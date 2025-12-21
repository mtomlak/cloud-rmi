<?php
namespace ProfiWeb\CloudRMI;

use ProfiWeb\CloudRMI\Exception\RemoteErrorException;
use ProfiWeb\CloudRMI\Exception\RemoteException;
use ProfiWeb\CloudRMI\Exception\RemoteMethodInvocationException;

/**
 * Remote Method Invocation client.
 *
 * @copyright Copyright (C) 2016-2020 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license Proprietary
 */
class CloudRMIClient
{
	protected const DEFAULT_MAX_LINE = 8192;
	protected const PROTOCOL_V1_MAX_LINE = 262144;

	protected $port = 8000;
	protected $host = 'localhost';
	protected $connection = null;
	protected $connTimeout = false;
	protected $readTimeout = 600;
	protected $handshakeReadTimeout = 10;
	protected $callReadTimeout = 30;
	protected $throttleTimeout = false;	//10;
	protected $throttleDuration = 0.01;
	protected $connStarted = null;
	protected $unixSocketPath = null;
	protected $auth = [];
	protected $capabilities = [];
	protected $advertisedMaxProtocol = 1;
	protected $supportsAsyncCapability = false;
	protected $payloadThreshold = null;
	protected $defaultPayloadEncoding = 'RAW';
	protected $protocolCtx = null;
	protected $protocolVersion = 1;
	protected $maxPayload = null;

	public function __construct($host = 'localhost', $port = 8000, $connTimeout = false, $readTimeout = 600, $throttleTimeout = false, $unixSocketPath = null)
	{
		$this->host = $host;
		$this->port = $port;
		$this->connTimeout = $connTimeout;
		$this->readTimeout = $readTimeout;
		$this->throttleTimeout = $throttleTimeout;
		$this->unixSocketPath = $this->normalizeUnixSocketPath($unixSocketPath);
		$this->protocolVersion = $this->getMinProtocolVersion();
	}

	public function connect()
	{
		$this->connStarted = time();
		$connectTimeout = ($this->connTimeout === false) ? $this->handshakeReadTimeout : $this->connTimeout;
		$socket = self::openSocket($this->host, $this->port, $connectTimeout, $this->unixSocketPath);
		
		$this->setConnection($this->makeNewConnection(
			$this->host,
			$this->port,
			$socket,
			$this->unixSocketPath
			));

		$previousTimeout = $this->readTimeout;
		$this->readTimeout = $this->handshakeReadTimeout;

		if (is_resource($socket))
			stream_set_timeout($socket, $this->handshakeReadTimeout);

		try {
			$banner = $this->connReadLn();

			if (!is_string($banner) || $banner === '')
				throw new RemoteMethodInvocationException("Cannot connect to RMI server: " . $this->describeEndpoint());

			if (($statusCode = $this->parseStatusCode($banner)) === false || $statusCode != 220)
				throw new RemoteMethodInvocationException("Error connecting to RMI server: $banner");

			$this->parseBanner($banner);
			$this->negotiateProtocol();
			$this->discoverCapabilities($banner);
			$this->updateCapabilityFlags();
		} finally {
			$this->readTimeout = $previousTimeout;

			if (is_resource($socket))
				stream_set_timeout($socket, $this->readTimeout);
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
		$command = ($options['async'] && $this->supportsAsyncCapability)
			? 'ACALL'
			: 'CALL';

		$request = $this->prepareCallRequest($command, $interface, $methodName, $parameters, $options);

		if ($request['use_payload']) {
			if ($this->sendPayloadCall($request) === false)
				return false;
		} else {
			if ($this->connWriteLn($request['line']) === false)
				return false;
		}

		$socket = $this->getConnection()->getSocket();
		$previousTimeout = $this->readTimeout;

		if (!is_null($this->callReadTimeout)) {
			$this->readTimeout = $this->callReadTimeout;

			if (is_resource($socket))
				stream_set_timeout($socket, $this->callReadTimeout);
		}

		try {
			$result = $this->readResponse();
		} finally {
			$this->readTimeout = $previousTimeout;

			if (is_resource($socket))
				stream_set_timeout($socket, $this->readTimeout);
		}

		if (!is_array($result) || !array_key_exists('line', $result))
			throw new RemoteMethodInvocationException('Invalid response from server');

		$code = $result['code'];

		if (in_array($code, [200, 500])) {
			try {
				if (!empty($result['is_payload'])) {
					$data = $this->decodePayloadResponse($result['encoding'], (string) $result['payload']);
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

			if ($code == 500) {
				if ($data instanceof RemoteException || $data instanceof RemoteErrorException)
					throw $data;

				throw new RemoteMethodInvocationException("Unrecognized exception type thrown on server", $data);
			}

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
		$this->connection = $connection;
		$this->restoreConnectionState($connection);

		return $this->connection;
	}

	public function getConnection()
	{
		return $this->connection;
	}

	protected function makeNewConnection($host, $port, $socket, $unixSocketPath = null)
	{
		return new CloudRMIConnection($host, $port, $socket, $unixSocketPath);
	}

	protected function restoreConnectionState(ConnectionInterface $connection)
	{
		if (!($connection instanceof CloudRMIConnection))
			return;

		if (!is_null($connection->getProtocolVersion()))
			$this->protocolVersion = (int) $connection->getProtocolVersion();

		if (!is_null($connection->getAdvertisedMaxProtocol()))
			$this->advertisedMaxProtocol = (int) $connection->getAdvertisedMaxProtocol();

		$capabilities = $connection->getCapabilities();
		if (!empty($capabilities))
			$this->capabilities = $capabilities;

		if (!is_null($connection->supportsAsyncCapability()))
			$this->supportsAsyncCapability = (bool) $connection->supportsAsyncCapability();
	}

	protected function persistConnectionState()
	{
		if (!($this->connection instanceof CloudRMIConnection))
			return;

		$this->connection->setProtocolVersion($this->protocolVersion);
		$this->connection->setAdvertisedMaxProtocol($this->advertisedMaxProtocol);
		$this->connection->setCapabilities($this->capabilities);
		$this->connection->setSupportsAsyncCapability($this->supportsAsyncCapability);
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

	protected function prepareCallRequest($command, $interface, $methodName, $parameters, array $options)
	{
		$encoding = ($options['encoding']) ? $options['encoding'] : $this->defaultPayloadEncoding;
		$payloadBytes = $this->normalizePayloadBytes($parameters, $encoding);
		$payloadLength = strlen($payloadBytes);
		$maxPayload = $this->getMaxPayloadLength();

		if ($payloadLength > $maxPayload)
			throw new RemoteMethodInvocationException('Payload exceeds configured max payload');

		$preferPayload = $this->shouldUsePayloadChannel($parameters, $options, $payloadLength);

		if (!empty($options['require_payload']))
			$preferPayload = true;

		$canUsePayload = ($this->protocolVersion >= 2);
		$inlinePayload = $this->encodePayload($parameters);
		$inlineLength = strlen("$command $interface $methodName " . $inlinePayload);

		if ($inlineLength >= $this->getMaxLineLength())
			$preferPayload = true;

		if ($preferPayload && !$canUsePayload) {
			if (!empty($options['require_payload']))
				throw new RemoteMethodInvocationException('Payload required but not supported by negotiated protocol version');

			if ($inlineLength >= $this->getMaxLineLength())
				throw new RemoteMethodInvocationException('Payload required but not supported by negotiated protocol version');

			$preferPayload = false;
		}

		if ($preferPayload) {
			$line = "$command $interface $methodName PAYLOAD $encoding $payloadLength";
			$this->validateOutboundPayload($line, null, $payloadBytes, $options);

			return [
				'use_payload' => true,
				'line' => $line,
				'payload' => $payloadBytes,
				'encoding' => $encoding,
			];
		}

		return [
			'use_payload' => false,
			'line' => "$command $interface $methodName " . $inlinePayload,
			'payload' => null,
			'encoding' => null,
		];
	}

	protected function getMaxLineLength()
	{
		// In protocol V1 ther MIGHT be lines longer than MAAX_LINE (8k),
		// but we enforce fefensive limit (256k)
		if ($this->protocolVersion < 2)
			return self::PROTOCOL_V1_MAX_LINE;

		if (defined('RMI_MAX_LINE'))
			return (int) RMI_MAX_LINE;

		return self::DEFAULT_MAX_LINE;
	}

	protected function getMaxPayloadLength()
	{
		if (!is_null($this->maxPayload))
			return (int) $this->maxPayload;

		if (defined('RMI_MAX_PAYLOAD'))
			return (int) RMI_MAX_PAYLOAD;

		if (defined('RMI_MAX_BUFFER'))
			return (int) RMI_MAX_BUFFER;

		return 10 * 1024 * 1024;
	}

	public function setMaxPayloadLength($bytes)
	{
		$this->maxPayload = (int) $bytes;
	}

	public function setHandshakeReadTimeout($seconds)
	{
		$this->handshakeReadTimeout = max(1, (int) $seconds);
	}

	public function setCallReadTimeout($seconds)
	{
		$this->callReadTimeout = is_null($seconds) ? null : max(1, (int) $seconds);
	}

	protected function getMinProtocolVersion()
	{
		return ($this->isUnixSocket()) ? 2 : 1;
	}

	protected function getInlinePayloadThreshold()
	{
		if (!is_null($this->payloadThreshold))
			return (int) $this->payloadThreshold;

		if (defined('RMI_INLINE_PAYLOAD_THRESHOLD'))
			return (int) RMI_INLINE_PAYLOAD_THRESHOLD;

		$maxLine = $this->getMaxLineLength();

		return (int) max(0, $maxLine - floor(($maxLine * 35) / 100) - 500);	// 35% base64 overhead for smaller strings + 500 bytes headspace
	}

	protected function isUnixSocket()
	{
		return (!is_null($this->unixSocketPath) && $this->unixSocketPath !== '');
	}

	protected function shouldUsePayloadChannel($parameters, array $options, $payloadLength = null)
	{
		if (!empty($options['payload']))
			return true;

		$threshold = $this->getInlinePayloadThreshold();

		if (!is_null($threshold)) {
			if (is_string($parameters) && strlen($parameters) > $threshold)
				return true;

			if (!is_null($payloadLength) && $payloadLength > $threshold)
				return true;
		}

		return false;
	}

	protected function sendPayloadCall(array $request)
	{
		if ($this->protocolVersion < 2)
			throw new RemoteMethodInvocationException('Payload required but not supported by negotiated protocol version');

		if ($this->connWriteLn($request['line']) === false)
			return false;

		if ($request['payload'] !== '' && $this->connWriteRaw($request['payload']) === false)
			return false;

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

		if (!function_exists('cloudrmi_protocol_feed') || !function_exists('cloudrmi_protocol_next_event'))
			return;

		if (function_exists('cloudrmi_protocol_reset'))
			cloudrmi_protocol_reset($ctx);

		$wire = '';

		if ($this->protocolVersion >= 2)
			$wire .= "PROTO 2\n";

		$wire .= $callLine . "\n";

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

			if (!isset($event['status']))
				return;

			if (defined('RMI_E_AGAIN') && $event['status'] === RMI_E_AGAIN) {
				throw new RemoteMethodInvocationException('Protocol parser expects more payload bytes');
			}

			if (defined('RMI_E_ERR') && $event['status'] === RMI_E_ERR) {
				$message = isset($event['err_message']) ? $event['err_message'] : 'Protocol error';
				throw new RemoteMethodInvocationException($message);
			}

			if (defined('RMI_OK') && $event['status'] === RMI_OK && isset($event['event']) && $event['event'] !== null)
				break;

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

			case 'RAW':
				return base64_decode($payload);

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

			case 'RAW':
			case 'PLAIN':
				return $payload;

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

	protected function describeEndpoint()
	{
		if ($this->isUnixSocket())
			return self::buildSocketTarget($this->host, $this->port, $this->unixSocketPath);

		return $this->host . ':' . $this->port;
	}

	protected function normalizeUnixSocketPath($path)
	{
		if ($path === null || $path === false)
			return null;

		$path = trim((string) $path);

		if ($path === '')
			return null;

		if (stripos($path, 'unix://') === 0)
			return substr($path, strlen('unix://'));

		return $path;
	}

	protected static function buildSocketTarget($host, $port, $unixSocketPath = null)
	{
		if (!is_null($unixSocketPath) && $unixSocketPath !== '') {
			if (stripos($unixSocketPath, 'unix://') === 0)
				return $unixSocketPath;

			return 'unix://' . $unixSocketPath;
		}

		return 'tcp://' . $host . ':' . $port;
	}

	protected static function openSocket($host, $port, $connTimeout, $unixSocketPath = null)
	{
		$errno = $errstr = null;
		$target = self::buildSocketTarget($host, $port, $unixSocketPath);

		if (!$socket = @stream_socket_client(
			$target,
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

	protected function parseBanner($banner)
	{
		$this->advertisedMaxProtocol = 1;
		$this->capabilities = [];

		if (preg_match('/PROTO=([0-9]+)/i', $banner, $match))
			$this->advertisedMaxProtocol = max(1, min(2, (int) $match[1]));

		if (preg_match('/PROTO=[^\s]*,([A-Z0-9_,\-]+)/i', $banner, $caps))
			$this->capabilities = $this->normalizeCapabilityTokens(explode(',', $caps[1]));
	}

	protected function negotiateProtocol()
	{
		$target = min(2, (int) $this->advertisedMaxProtocol);
		$minVersion = $this->getMinProtocolVersion();

		if ($target < $minVersion) {
			throw new RemoteMethodInvocationException('Negotiated protocol version is lower than required by the transport');
		}

		if ($target <= 1) {
			$this->protocolVersion = max(1, $minVersion);
			return;
		}

		$response = $this->rawCmd('PROTO ' . $target);
		$code = $this->parseStatusCode($response);

		if ($code == 200) {
			if (preg_match('/PROTO\s+([0-9]+)/i', (string) $response, $match))
				$this->protocolVersion = max($minVersion, (int) $match[1]);
			else
				$this->protocolVersion = max($minVersion, $target);
		} else {
			if ($minVersion > 1) {
				throw new RemoteMethodInvocationException('Failed to negotiate required protocol version');
			}

			$this->protocolVersion = 1;
		}
	}

	protected function discoverCapabilities($banner)
	{
		if (!empty($this->capabilities))
			return;

		$response = $this->rawCmd('CAPS');
		$code = $this->parseStatusCode($response);

		if ($code == 200) {
			$parts = explode(' ', $response, 2);
			$capsRaw = isset($parts[1]) ? trim($parts[1]) : '';
			if ($capsRaw !== '')
				$this->capabilities = $this->normalizeCapabilityTokens(preg_split('/[\s,]+/', $capsRaw));
		}
	}

	protected function normalizeCapabilityTokens(array $tokens)
	{
		$result = [];

		foreach ($tokens as $token) {
			$token = strtoupper(trim((string) $token));

			if ($token !== '')
				$result[] = $token;
		}

		return $result;
	}

	protected function updateCapabilityFlags()
	{
		$this->supportsAsyncCapability = in_array('ACALL', $this->capabilities);
		$this->persistConnectionState();
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
		$socket = $this->getConnection()->getSocket();
		$time = time();
		$maxLine = $this->getMaxLineLength();
		$buffer = '';

		while (is_resource($socket) && !feof($socket)) {
			if ($this->connTimeout && (time() - $this->connStarted > $this->connTimeout))
				return '322 Connection Timed Out: ' . $this->connTimeout;

			if (time() - $time > $this->readTimeout)
				return '322 Read Timed Out: ' . $this->readTimeout;

			$info = stream_get_meta_data($socket);

			if (isset($info['timed_out']) && $info['timed_out'])
				return '322 Socket Timed Out: ' . $this->readTimeout;

			$remaining = max(1, $maxLine - strlen($buffer) + 1);
			$chunk = fgets($socket, $remaining);

			if ($chunk === false) {
				$this->connCheckThrottle();
				continue;
			}

			if ($chunk === '') {
				$this->connCheckThrottle();
				continue;
			}

			$buffer .= $chunk;

			if (strpos($buffer, "\n") !== false || strlen($buffer) >= $maxLine) {
				break;
			}
		}

		return rtrim($buffer, "\r\n");
	}
	
	protected function connReadExact($bytes)	//, $consumeTrailingNewline = false)
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
/*
		if ($consumeTrailingNewline) {
			$next = stream_get_contents($this->getConnection()->getSocket(), 1);

			if ($next !== false && $next !== '' && $next !== "\n")
				$buff .= $next;
		}
*/
		return $buff;
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
				$lengthRaw = $parts[3];
				if (!ctype_digit((string) $lengthRaw)) {
					return ['line' => $line, 'code' => false];
				}

				$length = (int) $lengthRaw;
				$payload = '';

				if ($length < 0)
					return ['line' => $line, 'code' => false];

				$maxPayload = $this->getMaxPayloadLength();

				if ($length > $maxPayload) {
					$this->connReadExact($length);	// drain to keep connection in sync
					return [
						'line' => '505 Payload exceeds client limit',
						'code' => 505,
						'payload' => null,
						'payload_len' => null,
						'encoding' => null,
						'is_payload' => false,
					];
				}

				if ($length > 0) {
					$payload = $this->connReadExact($length);

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
