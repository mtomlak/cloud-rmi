<?php
namespace ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation;

/**
 * Simple implementation Remote Method Invocation server.
 *
 * @copyright Copyright (C) 2016-2020 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license Proprietary
 */
class NativeRMIClient
{
	protected $port = 8000;
	protected $host = 'localhost';
	protected $connection = null;
	protected $connTimeout = false;
	protected $readTimeout = 600;
	protected $throttleTimeout = false;	//10;
	protected $throttleDuration = 0.01;
	protected $connStarted = null;
	protected $auth = [];

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

		if (($statusCode = $this->parseStatusCode($banner)) !== false) {
			if ($statusCode != 220) 
				throw new RemoteMethodInvocationException("Error connecting to RMI server: $banner");	
		} else
			throw new RemoteMethodInvocationException("Cannot connect to RMI server: " 
				. $this->host . ':' . $this->port);

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
		
		$command = ($async ? 'ACALL' : 'CALL') . " $interface $methodName " . $this->encodePayload($parameters);
		$result = $this->rawCmd($command);

		// try to reconnect
		//if (!$result || $this->parseStatusCode($result) == 408) {
		//	$this->reconnect();
		//	$result = $this->rawCmd($command);
		//}

		// inspect the response and forward exceptions or errors
		if (in_array($code = $this->parseStatusCode($result), [200, 500])) {
			$msgParts = explode(' ', $result, 2);
			$payload = end($msgParts);

			try {
				$data = $this->decodePayload($payload);

			} catch (RemoteErrorException $e) {
				throw $e;

			} catch (\Exception $e) {
				throw new RemoteMethodInvocationException("Remote error: {$e->getMessage()}: $payload");	// most likely it's plaintext error
			}

			if ($code == 500)
				if ($data instanceof RemoteException || $data instanceof RemoteErrorException) {
					throw $data;

				} else 
					throw new RemoteMethodInvocationException("Unrecognized exception type thrown on server", $data);

			return $data;	// we know data consists of status code and a message
		}

		if (!$result) $result = '500 Internal RMI Server Error';

		throw new RemoteMethodInvocationException("Failed to call remote method: $result");
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

	protected function connReadLn()
	{
		$buff = null;
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
	
	protected function connReadChunk($bytes)
	{
		if (!feof($this->getConnection()->getSocket())) {
			$result = stream_get_contents($this->getConnection()->getSocket(), $bytes + 1);	// every chunk ends with "\n"

			if ($result === false)
				return false;
		}

		if (strlen($result) < $bytes)
			return '323 Content Truncated';

		return unserialize(trim($result)); // this caused issue with 8.3 on hackbook, likely linefeed at the end
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
}
