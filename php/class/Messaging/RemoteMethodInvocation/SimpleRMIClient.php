<?php
namespace ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation;

/**
 * Simple implementation Remote Method Invocation server.
 *
 * @copyright Copyright (C) 2016-2020 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license Proprietary
 */
class SimpleRMIClient
{
	protected const DEFAULT_MAX_LINE = 8192;
	protected const INLINE_PAYLOAD_OVERHEAD_PERCENT = 35;
	protected const INLINE_PAYLOAD_HEADROOM = 500;

	protected $port = 8000;
	protected $host = 'localhost';
	protected $connection = null;
	protected $connTimeout = false;
	protected $readTimeout = 600;
	protected $throttleTimeout = false;	//10;
	protected $throttleDuration = 0.01;
	protected $connStarted = null;
	protected $auth = [];

	protected $payloadThreshold = null;
	protected $defaultPayloadEncoding = 'RAW';
	protected $protocolCtx = null;
	protected $protocolVersion = 1;

	public function __construct($host = 'localhost', $port = 8000, $connTimeout = false, $readTimeout = 600, $throttleTimeout = false)
	{
		$this->host = $host;
		$this->port = $port;
		$this->connTimeout = $connTimeout;
		$this->readTimeout = $readTimeout;
		$this->throttleTimeout = $throttleTimeout;
	}

	public function connect()
	{
		$this->connStarted = time();
		$socket = self::openSocket($this->host, $this->port, $this->readTimeout);
		
		$this->setConnection(new SimpleRMIConnection(
			$this->host,
			$this->port,
			$socket
			));

                $banner = $this->connReadLn();
                if (!is_string($banner) || $banner === '')
                        throw new RemoteMethodInvocationException("Cannot connect to RMI server: "
                                        . $this->host . ':' . $this->port);

                                if (($statusCode = $this->parseStatusCode($banner)) !== false) {
						if ($statusCode != 220)
								throw new RemoteMethodInvocationException("Error connecting to RMI server: $banner");
				} else
						throw new RemoteMethodInvocationException("Cannot connect to RMI server: "
								. $this->host . ':' . $this->port);

				if (stripos($banner, 'PROTO=2') !== false) {
						$response = $this->rawCmd('PROTO 2');
						if ($this->parseStatusCode($response) == 200)
								$this->protocolVersion = 2;
				}

				return $this->getConnection();
		}

	public function disconnect()
	{
		if (!$this->isConnected())
			return;

		$this->rawCmd('QUIT');
		static::closeSocket($this->getConnection()->getSocket());
		$this->connection = null;
	}

	public function isConnected()
	{
		return (!is_null($this->connection));
	}


	public function authByToken($token)
	{
		$result = $this->rawCmd("AUTH TOKEN $token");

		if ($this->parseStatusCode($result) == 200) {
			$this->saveLastAuth('token', ['token' => $token]);

			return true;
		}

		throw new RemoteMethodInvocationException("RMI authentication error: $result");	
	}

	public function callRemoteMethod($interface, $methodName, $parameters, $async = false)
	{
		if ($interface[0] != '\\')	// make sure we have absolute classname
			$interface = "\\$interface";

		$options = $this->normalizeCallOptions($async);
		$command = $options['async'] ? 'ACALL' : 'CALL';
		$preferPayload = $this->shouldUsePayloadChannel($parameters, $options);
		if (!empty($options['require_payload']))
			$preferPayload = true;
		$payloadChannelAvailable = $this->canUsePayloadChannel($options);
		$inlinePayload = null;
		$inlineLength = null;

		if (!$preferPayload || !$payloadChannelAvailable) {
			$inlinePayload = $this->encodePayload($parameters);
			$inlineLength = strlen("$command $interface $methodName " . $inlinePayload);

			if (!$preferPayload && $inlineLength >= $this->getMaxLineLength())
				$preferPayload = true;
		}

		if ($preferPayload && !$payloadChannelAvailable) {
			if (!empty($options['require_payload']))
				throw new RemoteMethodInvocationException('Payload required but not supported by negotiated protocol version');

			if (is_null($inlinePayload)) {
				$inlinePayload = $this->encodePayload($parameters);
				$inlineLength = strlen("$command $interface $methodName " . $inlinePayload);
			}

			if ($inlineLength >= $this->getMaxLineLength())
				throw new RemoteMethodInvocationException('Payload required but not supported by negotiated protocol version');

			$preferPayload = false;
		}

		if ($preferPayload) {
			if ($this->sendPayloadCall($interface, $methodName, $parameters, $options) === false)
				return false;
			$result = $this->readResponse();
		} else {
			$commandLine = "$command $interface $methodName " . $inlinePayload;
			if ($this->connWriteLn($commandLine) === false)
				return false;
			$result = $this->readResponse();
		}

		if (!is_array($result) || !array_key_exists('line', $result))
			throw new RemoteMethodInvocationException('Invalid response from server');

		$code = $result['code'];

		if (in_array($code, [200, 500])) {
			try {
				if (!empty($result['is_payload'])) {
					$data = $this->decodePayloadResponse($result['encoding'], (string)$result['payload']);
				} else {
					$msgParts = explode(' ', $result['line'], 2);
					$payload = end($msgParts);
					$data = $this->decodePayload($payload);
				}
			} catch (RemoteErrorException $e) {
				throw $e;

			} catch (\Exception $e) {
				$payloadString = isset($payload) ? $payload : ($result['payload'] ?? '');
				throw new RemoteMethodInvocationException("Remote error: {$e->getMessage()}: {$payloadString}");
			}

			if ($code == 500)
				if ($data instanceof RemoteException || $data instanceof RemoteErrorException) {
					throw $data;

				} else
					throw new RemoteMethodInvocationException("Unrecognized exception type thrown on server", $data);

			return $data;	// we know data consists of status code and a message
		}

                if (empty($result['line']))
                        $result['line'] = '500 Internal RMI Server Error';

                throw new RemoteMethodInvocationException("Failed to call remote method: " . $result['line']);
        }

        public function call($interface, $methodName, $parameters, $async = false)
        {
                return $this->callRemoteMethod($interface, $methodName, $parameters, $async);
        }
        public function setConnection(ConnectionInterface $connection)
        {
                return $this->connection = $connection;
        }

	public function getConnection()
	{
		return $this->connection;
	}

	protected function saveLastAuth($method, $data)
	{
		$this->auth[] = [$method, $data];
	}

	protected function repeatLastAuth()
	{
		if (empty($this->auth))
			return false;

		list($method, $data) = end($this->auth);

		switch ($method) {
			case 'token':
			default:
				$this->authByToken($data['token']);
				break;
		}

		return true;
	}

	protected function reconnect()
	{
		$this->disconnect();
		$this->connect();
		$this->repeatLastAuth();
	}

	protected function normalizeCallOptions($async)
	{
                $options = [
                        'async' => false,
                        'payload' => false,
                        'encoding' => null,
                        'require_payload' => false,
                ];

                if (is_array($async)) {
			if (array_key_exists('async', $async))
				$options['async'] = (bool) $async['async'];

			if (array_key_exists('payload', $async))
				$options['payload'] = (bool) $async['payload'];

			if (isset($async['encoding']))
				$options['encoding'] = $async['encoding'];

			if (isset($async['require_payload']))
				$options['require_payload'] = (bool) $async['require_payload'];
                } else {
                        $options['async'] = (bool) $async;
                }

		return $options;
	}

	protected function getMaxLineLength()
	{
		if (defined('RMI_MAX_LINE'))
			return (int) RMI_MAX_LINE;

		return self::DEFAULT_MAX_LINE;
	}

	protected function getInlinePayloadThreshold()
	{
		if (!is_null($this->payloadThreshold))
			return (int) $this->payloadThreshold;

		if (defined('RMI_INLINE_PAYLOAD_THRESHOLD'))
			return (int) RMI_INLINE_PAYLOAD_THRESHOLD;

		$maxLine = $this->getMaxLineLength();
		return (int) max(0, $maxLine - floor(($maxLine * self::INLINE_PAYLOAD_OVERHEAD_PERCENT) / 100) - self::INLINE_PAYLOAD_HEADROOM);
	}

	protected function canUsePayloadChannel(array $options)
	{
		return $this->protocolVersion >= 2;
	}

	protected function shouldUsePayloadChannel($parameters, array $options)
	{
		if (!empty($options['payload']))
			return true;

		$threshold = $this->getInlinePayloadThreshold();

		if (!is_null($threshold) && is_string($parameters) && strlen($parameters) > $threshold)
			return true;

		return false;
	}

		protected function sendPayloadCall($interface, $methodName, $parameters, array $options)
		{
				$encoding = ($options['encoding']) ? $options['encoding'] : $this->defaultPayloadEncoding;
				$payload = $this->normalizePayloadBytes($parameters, $encoding);
				$callLine = ($options['async'] ? 'ACALL' : 'CALL') . " $interface $methodName";

                                if ($this->protocolVersion >= 2) {
                                                $callLine .= " $encoding " . strlen($payload);

                                                if ($this->connWriteLn($callLine) === false)
                                                                return false;

                                                if ($payload !== '' && $this->connWriteRaw($payload) === false)
                                                                return false;
                                } else {
						$payloadLine = "PAYLOAD $encoding " . strlen($payload);

						$this->validateOutboundPayload($callLine, $payloadLine, $payload, $options);

						if ($this->connWriteLn($callLine) === false)
								return false;

						if ($this->connWriteLn($payloadLine) === false)
								return false;

						if ($payload !== '' && $this->connWriteRaw($payload) === false)
								return false;
				}

				return true;
		}

	protected function normalizePayloadBytes($parameters, &$encoding)
	{
		$encoding = strtoupper((string) $encoding);

		if (empty($encoding))
			$encoding = $this->defaultPayloadEncoding;

		if (is_string($parameters))
			return $parameters;

		if ($encoding == $this->defaultPayloadEncoding || $encoding == 'RAW')
			$encoding = 'PHP';

		return serialize($parameters);
	}

		protected function validateOutboundPayload($callLine, $payloadLine, $payload, array $options)
		{
				if (!$ctx = $this->getProtocolContext())
						return;

		if (function_exists('cloudrmi_protocol_reset'))
			cloudrmi_protocol_reset($ctx);

				if (!function_exists('cloudrmi_protocol_feed') || !function_exists('cloudrmi_protocol_next_event'))
						return;

				$wire = $callLine . "\n";
				if (!is_null($payloadLine))
						$wire .= $payloadLine . "\n";
				$wire .= $payload;

				$feed = cloudrmi_protocol_feed($ctx, $wire);
		if (isset($feed['status']) && defined('RMI_E_ERR') && $feed['status'] === RMI_E_ERR) {
			$message = isset($feed['err_message']) ? $feed['err_message'] : 'Protocol error';
			throw new RemoteMethodInvocationException($message);
		}

		while (true) {
			$event = cloudrmi_protocol_next_event($ctx);
			if (!isset($event['status'])) {
				return;
			}

			if (defined('RMI_E_AGAIN') && $event['status'] === RMI_E_AGAIN) {
				throw new RemoteMethodInvocationException('Protocol parser expects more payload bytes');
			}

			if (defined('RMI_E_ERR') && $event['status'] === RMI_E_ERR) {
				$message = isset($event['err_message']) ? $event['err_message'] : 'Protocol error';
				throw new RemoteMethodInvocationException($message);
			}

                        if (defined('RMI_OK') && $event['status'] === RMI_OK && isset($event['event']) && $event['event'] !== null) {
                                break;
                        }

			if (!defined('RMI_E_AGAIN') || $event['status'] !== RMI_E_AGAIN)
				break;
		}
	}

	protected function getProtocolContext()
	{
		if (!function_exists('cloudrmi_protocol_create'))
			return null;

		if (is_null($this->protocolCtx))
			$this->protocolCtx = cloudrmi_protocol_create();

		return $this->protocolCtx;
	}

	protected function encodePayload($payload)
	{
		// If payload is simple string, don't serialize
		if (is_string($payload))
			return base64_encode($payload);

		else
			return 'PHP:' . base64_encode(serialize($payload));
	}

	protected function decodePayload($rawPayload)
	{
		list($encoding, $payload) = (strpos($rawPayload, ':'))
			? explode(':', $rawPayload, 2)
			: ['PLAIN', $rawPayload];	// default encoding is plaintext (base64)

		switch(strtoupper((string) $encoding)) {
			case 'PHP':
				return unserialize(base64_decode($payload));

			case 'JSON':
				return json_decode(base64_decode($payload));

			// Space case ofencoding that launches one subsequent read from the socket
			case 'BLOB':
				if (!is_numeric($payload))
					RemoteMethodInvocationException("Invalid BLOB size received");

				return $this->connReadChunk($payload);

			case 'PLAIN':	// treated as default if no encoding is specified
				if (($data = base64_decode($payload, true)) === false)
					return $payload;	// non-encoded plaintext fallback
				else
					return $data;

			case 'ERROR':	// type of short plaintext
				if (($data = base64_decode($payload, true)) === false)
					$data = $payload;	// non-encoded plaintext fallback

				throw new RemoteErrorException($data);

			default:
				throw new RemoteMethodInvocationException("Unsupported RMI encoding received: $encoding");
		}
	}

	protected function decodePayloadResponse($encoding, $payload)
	{
		switch(strtoupper((string) $encoding)) {
			case 'PHP':
				return unserialize($payload);

			case 'JSON':
				return json_decode($payload);

			case 'ERROR':
				throw new RemoteErrorException($payload);

			default:
				return $payload;
		}
	}

	protected function rawCmd($cmd)
	{
		if ($this->connWriteLn($cmd) === false)
			return false;

		return $this->connReadLn();
	}

	protected static function openSocket($host, $port, $connTimeout)
	{
		$errno = $errstr = null;

		if (!$socket = @stream_socket_client(
			'tcp://' . $host . ':' . $port, 
			$errno, 
			$errstr, 
			$connTimeout
			))
			throw new RemoteMethodInvocationException("Cannot connect to RMI server: $errstr ($errno)");

		stream_set_blocking($socket, true);
		stream_set_timeout($socket, $connTimeout);

		return $socket;
	}

	protected static function closeSocket($socket)
	{
		@fclose($socket);
	}

	protected function parseStatusCode($message)
	{
		if (!empty($message)) {
			$parts = explode(' ', $message, 2);

			if (is_numeric($parts[0]))
				return (int) $parts[0];
		}

		return false;
	}

	protected function connWriteLn($line)
	{
		if (!is_resource($this->getConnection()->getSocket()) || feof($this->getConnection()->getSocket()))
			return false;

		$info = stream_get_meta_data($this->getConnection()->getSocket());

		if (isset($info['timed_out']) && $info['timed_out'])
			return false;

		return fwrite($this->getConnection()->getSocket(), "$line\r\n");
	}

	protected function connWriteRaw($bytes)
	{
		if (!is_resource($this->getConnection()->getSocket()) || feof($this->getConnection()->getSocket()))
			return false;

		$info = stream_get_meta_data($this->getConnection()->getSocket());

		if (isset($info['timed_out']) && $info['timed_out'])
			return false;

		$written = 0;
		$length = strlen($bytes);

		while ($written < $length) {
			$result = fwrite($this->getConnection()->getSocket(), substr($bytes, $written));

			if ($result === false)
				return false;

			$written += $result;
		}

		return $written;
	}

	protected function connReadLn()
	{
                $buff = '';
		$result = 0;
		$time = time();
		$info = stream_get_meta_data($this->getConnection()->getSocket());

		while (is_resource($this->getConnection()->getSocket()) && !feof($this->getConnection()->getSocket()) && strpos($buff, "\n") === false) {
			if ($this->connTimeout && (time() - $this->connStarted > $this->connTimeout))
				return '322 Connection Timed Out: ' . $this->connTimeout;

			if (time() - $time > $this->readTimeout)
				return '322 Read Timed Out: ' . $this->readTimeout;

			if (isset($info['timed_out']) && $info['timed_out'])
				return '322 Socket Timed Out: ' . $this->readTimeout;

			$this->connCheckThrottle();
			
			$result = stream_get_contents($this->getConnection()->getSocket(), 1);

			if ($result === false)
				return false;

			$buff .= $result;
		}

		return trim($buff);
	}
	
	protected function connReadExact($bytes, $consumeTrailingNewline = false)
	{
		$buff = '';
		$time = time();

		while (strlen($buff) < $bytes) {
			if ($this->connTimeout && (time() - $this->connStarted > $this->connTimeout))
				return '322 Connection Timed Out: ' . $this->connTimeout;

			if (time() - $time > $this->readTimeout)
				return '322 Read Timed Out: ' . $this->readTimeout;

			if (!is_resource($this->getConnection()->getSocket()) || feof($this->getConnection()->getSocket()))
				return false;

			$info = stream_get_meta_data($this->getConnection()->getSocket());

			if (isset($info['timed_out']) && $info['timed_out'])
				return '322 Socket Timed Out: ' . $this->readTimeout;

			$chunk = stream_get_contents($this->getConnection()->getSocket(), $bytes - strlen($buff));

			if ($chunk === false)
				return false;

			if ($chunk === '') {
				$this->connCheckThrottle();
				continue;
			}

			$buff .= $chunk;
		}

		if ($consumeTrailingNewline) {
			$next = stream_get_contents($this->getConnection()->getSocket(), 1);

			if ($next !== false && $next !== '' && $next !== "\n")
				$buff .= $next;
		}

		return $buff;
	}

		protected function connReadChunk($bytes)
		{
				$result = $this->connReadExact($bytes, true);

				if ($result === false)
						return false;

		if (!is_string($result))
			return $result;

				if (strlen($result) < $bytes)
						return '323 Content Truncated';

				return unserialize($result);
		}

		protected function readResponse()
		{
				$line = $this->connReadLn();

				if ($line === false)
						return ['line' => '', 'code' => false];

				$response = [
						'line' => $line,
						'code' => $this->parseStatusCode($line),
						'payload' => null,
						'payload_len' => null,
						'encoding' => null,
						'is_payload' => false,
				];

                                if ($this->protocolVersion >= 2 && $response['code'] !== false) {
                                                $parts = preg_split('/\s+/', trim($line));

                                                if (count($parts) >= 4 && strtoupper($parts[1]) === 'PAYLOAD') {
                                                                $length = (int) $parts[3];
                                                                $payload = '';

                                                                if ($length > 0) {
                                                                                $payload = $this->connReadExact($length, false);
                                                                                if (!is_string($payload) || strlen($payload) != $length)
                                                                                                return ['line' => $line, 'code' => false];
                                                                }

                                                                $response['encoding'] = $parts[2];
                                                                $response['payload_len'] = $length;
                                                                $response['payload'] = $payload;
                                                                $response['is_payload'] = true;
                                                }
                                }

				return $response;
		}

	protected function connCheckThrottle()
	{
		$maxDuration = 5;

		if ($this->throttleTimeout === false || !$this->connStarted)
			return;

		if (time() - $this->connStarted > $this->throttleTimeout) {
			// Slowly increase up to maximum limit
			if ($this->throttleDuration * 2 > $maxDuration)
				$this->throttleDuration = $maxDuration;

			elseif ($this->throttleDuration < $maxDuration)
				$this->throttleDuration *= 2;

			usleep($this->throttleDuration * 1000000);
		}
	}

	public function __destruct()
	{
		if (!is_null($this->protocolCtx) && function_exists('cloudrmi_protocol_destroy'))
			cloudrmi_protocol_destroy($this->protocolCtx);
	}

}
