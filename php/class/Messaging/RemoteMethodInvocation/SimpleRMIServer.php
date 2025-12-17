<?php
namespace ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation;

use ProfiWeb\Cloud\Infrastructure\System\ProcessControl\ProcessFork;

/**
 * Simple implementation of Remote Method Invocation server.
 *
 * @copyright Copyright (C) 2016 - 2024 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license Proprietary
 */
class SimpleRMIServer
{
	protected $serverSocket;
	// Configuration parameters
	protected $port = 8000;
	protected $host = '0.0.0.0';
	protected $recoveryToken = null;
	protected $contextName = 'dev.rmi';
	protected $crashlog = '/var/log/cloud.{contextName}.crash.log';
	protected $disableFork = false;
	// Other properties
	protected $pid;
	protected $severSocket = null;
	protected $doShutdown = false;
	protected $isAuthenticated = false;
	protected $username = null;
	protected $readTimeout = 300;
	protected $highReadTimeout = 300;
	protected $noForkReadTimeout = 300;
	protected $childTimeout = 3600;	// default is 6h, use short one for web-facing servers
	protected $childThrottle = false;
	protected $childNicenessThrottle = [30 => 5, 35 => 10, 40 => 15, 45 => 20];	// X => Y: sets child niceness to Y at Xth second
	protected $childIoNicenessThrottle = [30 => 2, 45 => 3];	// see "man ionice"
	protected $throttleTimeout = false;
	protected $throttleDuration = 0.01;
	protected $connStarted = null;
        protected $bindings;
        protected $startedAt;
        protected $proc;
        protected $serverPid;
        protected $requireAuth = true;
	// Protocol-related
	protected $maxPlainStringBytes = 1024;
        protected $protocolVersion = 1;
        protected $maxProtocolVersion = 2;
        protected $maxLineLength = 8192;

        public function __construct()
        {
                $this->bindings = array();

                if (!$this->hasForkSupport())
                        $this->readTimeout = $this->noForkReadTimeout;
        }

        public function setMaxProtocolVersion($version)
        {
                $version = (int) $version;

                if ($version < 1)
                        $version = 1;

                $this->maxProtocolVersion = $version;
        }

        public function setRequireAuth($requireAuth)
        {
                $this->requireAuth = (bool) $requireAuth;
        }

	public function start()
	{
		// note that the loop will work even without fork support
		$this->registerShutdownHandler();
		$this->openServerSocket();
		$this->startedAt = time();
		$this->serverPid = posix_getpid();
		$this->log($this->contextName . " service ready");

                if ($this->hasForkSupport())
                        $this->proc = new ProcessFork();

                // main program loop
                while (!$this->doShutdown) {
                        $this->acceptNewConnections();

                        if ($this->hasForkSupport() && $this->proc instanceof ProcessFork) {
                                if (version_compare(PHP_VERSION, '7.1.0', '<'))
                                        $this->proc->dispatchSignals();

                                $this->proc->collectChildren();
                        }
                }

		$this->closeServerSocket();
		$this->doShutdown = false; // in case it will be started again
	}

	public function bind($interface, &$objectRef) 
	{
		if (!interface_exists($interface))
			throw new \InvalidArgumentException("Undefined interface $interface");

		if (!is_object($objectRef) || !$objectRef instanceof $interface)
			throw new \InvalidArgumentException("Given object does not implement "
				. "specified interface: $interface");

		if ($interface[0] != '\\')	// make sure name is absolute classname
			$interface = "\\$interface";

		$this->bindings[$interface] = $objectRef;
	}

	public function handleProccessShutdown()
	{
                if (($this->proc && $this->proc->isParent()) || !$this->hasForkSupport()) {
                        $this->closeServerSocket();
                }

		if (!$this->doShutdown)
			$this->handleCrash();
	}

        protected function acceptNewConnections()
        {
                if ($conn = @stream_socket_accept($this->serverSocket))
                        $this->handleConnection($conn);
        }

        protected function getMaxProtocolVersion()
        {
                return $this->maxProtocolVersion;
        }

	protected function handleConnection($socket)
	{
		if ($this->hasForkSupport()) {
			//$this->proc = new ProcessFork();
			$pid = $this->proc->fork();

			if ($this->proc->isParent()) {
				$this->log("Forked new child proccess with PID $pid");
				$this->closeSocket($socket);
				return;

			}

			if (!$watchdogPid = $this->forkChildWatchdog(getmypid()))
				return;		// we're the watchdog proccess, end here
		} else
			$this->log("Warning: Fork support is not enabled");

		stream_set_timeout($socket, $this->readTimeout);
		stream_set_blocking($socket, true);
                $this->connWriteLn($socket, "220 " . $this->contextName . " Server Ready EXT PROTO=" . $this->getMaxProtocolVersion());
                $this->isAuthenticated = !$this->requireAuth;
                $this->username = null;
                $this->protocolVersion = 1;
                $this->connStarted = time();

		do {
			$input = $this->connReadLn($socket);

			if ($input === false)
				break;

			$continue = $this->handleCommand($socket, $input);

		} while ($continue !== false);

		$this->closeSocket($socket);

		if ($this->hasForkSupport()) {
			posix_kill($watchdogPid, SIGTERM);
			$this->log("Shutting child proccess " . getmypid());
			$this->doShutdown = true;
			return $this->proc->join();	// exit child proccess
		}
	}

	protected function getBinding($interface)
	{
		return (isset($this->bindings[$interface])) 
			? $this->bindings[$interface]
			: null;
	}

	protected function getBindingMethods($interface)
	{
		if (!interface_exists($interface))
			throw new \InvalidArgumentException("Undefined interface $interface");

		return get_class_methods($interface);
	}

	/**
	 * Handles a command. Should return false when we want to close connection.
	 * 
	 */
	protected function handleCommand(&$socket, $commandStr)
	{
		$parts = explode(' ', $commandStr, 2);

		$command = $parts[0];
		$arg = (isset($parts[1])) ? $parts[1] : null;
		
		switch(strtolower($command)) {
			case 'auth':
				if (!$result = $this->handleAuthCommand($arg)) {
					$this->connWriteLn($socket, '401 Unauthorized');	

					return false;
				}

				$this->connWriteLn($socket, $result);
				break;

			case 'call':
				$this->handleCallCommand($socket, $arg);
				break;

			case 'acall':	// async call
				$this->handleACallCommand($socket, $arg);
				break;

			case 'proto':
                                list($version, $caps) = $this->parseProtoCommand($arg);
                                $this->protocolVersion = $version;
                                $this->connWriteLn($socket, "200 PROTO $version");
				break;

			case 'help':
				$this->connWriteLn($socket, "200 Supported commands:\n"
					. "ACALL\nAUTH\nCALL\nHELP\nIFACES\nNOOP\nSHUTDOWN\nTIME\nUPTIME\nQUIT\n");
				break;

			// list of supported extensions - interfaces
			case 'ifaces':
				$this->readTimeout = $this->highReadTimeout;
				$this->connWriteLn($socket, "200 Supported CALL interfaces:\n"
					. implode("\n", array_keys($this->bindings)) . "\n");
				break;

			case 'noop':
				$this->readTimeout = $this->highReadTimeout;
				$this->connWriteLn($socket, "200 OK");
				break;

			case 'shutdown':
				if (!$this->isAuthenticated) {
					$this->connWriteLn($socket, '401 Unauthorized');	
					break;
				}
				$this->doShutdown = true;
				$this->connWriteLn($socket, "221 Server shutting down");
				\posix_kill($this->serverPid, SIGTERM);

				return false;

			case 'time':
				$this->connWriteLn($socket, date("200 D, d M y H:i:s O"));	// RFC822
				break;

			case 'uptime':
				if (!$this->isAuthenticated) {
					$this->connWriteLn($socket, '401 Unauthorized');	
					break;
				}
				$diff = time() - $this->startedAt;
				$this->connWriteLn($socket, sprintf(
					"200 Server up for %s days, %s hrs, %s min, %s sec",
					(int) floor($diff / 86400),
					(int) floor(($diff % 86400) / 3600),
					(int) floor(($diff % 3600) / 60),
					(int) $diff % 60
					));
				break;

			case 'debug':
				if (!$this->isAuthenticated) {
					$this->connWriteLn($socket, '401 Unauthorized');	
					break;
				}
				$this->readTimeout = $this->highReadTimeout;
				$this->connWriteLn($socket, "200 Debug information:");
				$this->connWriteLn($socket, "Server build:");
				$this->connWriteLn($socket, "\tLanguage:\tPHP " . phpversion());
				$this->connWriteLn($socket, "\tKernel API:\t" . (($this->hasForkSupport()) ? 'POSIX' : 'none' ));
				$this->connWriteLn($socket, "\tProxy support:\tNO");
				$this->connWriteLn($socket, "Runtime:");
				$this->connWriteLn($socket, "\tServer proccess PID:\t" . $this->serverPid);
				//$this->connWriteLn($socket, "\tConfiguration file path:\t%s");
				break;

			case 'quit':
			case 'exit':
				$this->connWriteLn($socket, "221 Bye");

				return false;

		case '__timeout':
			$this->connWriteLn($socket, "408 Request Timeout");

			return false;

		case '__overflow':
			$this->connWriteLn($socket, "400 Bad Request");

			return false;

			case '':
				break;

			default:
				$this->connWriteLn($socket, "400 Bad Request");
				break;
		}

		return null;
	}

	protected function handleCallCommand(&$socket, $arg)
	{
                if ($this->requireAuth && !$this->isAuthenticated) {
                        $this->connWriteLn($socket, '401 Unauthorized');
                        return;
                }

		list($iface, $method, $params, $payloadMode) = $this->parseCallArguments($socket, $arg);

		if ($iface === null || $method === null) {
			$this->connWriteLn($socket, "400 Bad Request: Missing argument");
			return;
		}

		if (!$binding = $this->getBinding($iface)) {
			$this->connWriteLn($socket, "404 Not Found");
			return;
		}

		if (!in_array($method, $this->getBindingMethods($iface))) {
			$this->connWriteLn($socket, "405 Method Not Allowed");
			return;
		}

		try {
			$result = @call_user_func_array(
				array($binding, $method),
				is_array($params) ? $params : [$params]
				);

		} catch (\Throwable $e) {
			$this->log(' ! ' . get_class($e) . ': ' . $e->getMessage());

			$ex = new RemoteException(
				$e,
				$this->contextName,
				$this->host,
				$this->port
				);

                        if ($payloadMode && $this->protocolVersion >= 2) {
                                $this->sendPayloadResponse($socket, 500, $ex);
                                return;
                        }

                        $this->connWriteLn($socket, "500 " . $this->encodePayload($ex));
			return;
		}

                if ($payloadMode && $this->protocolVersion >= 2) {
                        $this->sendPayloadResponse($socket, 200, $result);
                        return;
                }

		try {
			$encodedResult = $this->encodePayload($result);

		} catch (\Throwable $e) {
			$this->connWriteLn($socket, "500 Unable to transmit result: " . $iface . '::' . $method . ': ' . $e->getMessage());
			return;
		}

		$this->connWriteLn($socket, "200 " . $encodedResult);
	}

	protected function handleACallCommand(&$socket, $arg)
	{
                if ($this->requireAuth && !$this->isAuthenticated) {
                        $this->connWriteLn($socket, '401 Unauthorized');
                        return;
                }

		list($iface, $method, $params, $payloadMode) = $this->parseCallArguments($socket, $arg);

		if ($iface === null || $method === null) {
			$this->connWriteLn($socket, "400 Bad Request: Missing argument");
			return;
		}

		if (!$binding = $this->getBinding($iface)) {
			$this->connWriteLn($socket, "404 Not Found");
			return;
		}

		if (!in_array($method, $this->getBindingMethods($iface))) {
			$this->connWriteLn($socket, "405 Method Not Allowed: $method: ".implode(', ', $this->getBindingMethods($iface)));
			return;
		}

		if (!$this->hasForkSupport()) {
			$this->connWriteLn($socket, '405 Method Not Allowed: No fork support');
			return;
		}

		$proc = new ProcessFork();
		$pid = $proc->fork();

		if ($proc->isParent()) {
			$this->log("Forked new async child proccess with PID $pid");
			$this->connWriteLn($socket, "200 Call Executed");
			return;
		}

		if (!$watchdogPid = $this->forkChildWatchdog(getmypid()))
			return;

		try {
			@call_user_func_array(
				array($binding, $method),
				is_array($params) ? $params : [$params]
				);
		} catch (\Throwable $e) {
			$this->log(' ! ' . get_class($e) . ': ' . $e->getMessage());
		}

		$this->log("Finished handling async call: $arg");

		$this->doShutdown = true;
		return $this->proc->join();
	}

	protected function checkCallCommand($args)
	{

		return null;
	}

	protected function handleAuthCommand($arg)
	{
		if ($this->isAuthenticated)
			return '200 Already logged in as: ' . $this->username;

		$args = explode(' ', trim($arg), 2);

		if (count($args) < 2)
			return "400 Bad Request: Supported AUTH methods: TOKEN, LOGIN";	// . serialize($args);

		list($method, $parameter) = $args;

		// todo: refractor this
		switch(strtolower($method)) {
			case 'token':
				$token = trim($parameter);
				// rapid development - debug purposes
				if (!empty($this->recoveryToken) && $this->recoveryToken == $token) {
					$this->isAuthenticated = true;
					$this->username = 'RecoveryUser';

					return "200 Hello " . $this->username;
				} else
					return false;

			case 'login':
				if (trim($parameter) == 'debug debug123') {
					$this->isAuthenticated = true;
					$this->username = 'DebugUser';

					return "200 Hello " . $this->username;
				} else
					return false;

			default:
				return "400 Bad Request: Unsupported AUTH method";	
		}

		return "400 Bad Request:";
	}

	protected function parseProtoCommand($arg)
	{
		$version = 1;

                if (!is_null($arg)) {
                        $parts = explode(' ', trim((string) $arg), 2);
                        if (isset($parts[0]) && is_numeric($parts[0]))
                                $version = (int) $parts[0];
                }

                if ($version < 1)
                        $version = 1;

                if ($version > $this->getMaxProtocolVersion())
                        $version = $this->getMaxProtocolVersion();

                return [$version, null];
        }

	protected function parseCallArguments(&$socket, $arg)
	{
		$arg = trim((string) $arg);

                if ($this->protocolVersion >= 2) {
			$parts = preg_split('/\s+/', $arg, 4);

			if (count($parts) == 4 && is_numeric($parts[3])) {
				list($iface, $method, $enc, $lenStr) = $parts;
				$len = (int) $lenStr;

				if ($len < 0) {
					return [null, null, null, true];
				}

				if ($iface && $iface[0] != '\\')
					$iface = "\\$iface";

				$payload = ($len > 0) ? $this->connReadExact($socket, $len) : '';

				if ($payload === false || !is_string($payload))
					return [null, null, null, true];

				$params = $this->decodePayloadBytes($enc, $payload);

				return [$iface, $method, $params, true];
			}
		}

		$args = explode(' ', $arg, 3);

		if (count($args) != 3)
			return [null, null, null, false];

		list($iface, $method, $vars) = $args;

		if ($iface && $iface[0] != '\\')
			$iface = "\\$iface";

		return [$iface, $method, $this->decodeMethodArgs($vars), false];
	}

	protected function decodePayloadBytes($encoding, $payload)
	{
		$encoding = strtoupper((string) $encoding);

		switch ($encoding) {
			case 'RAW':
			case 'PLAIN':
				return (string) $payload;

			case 'PHP':
				return unserialize($payload);

			case 'JSON':
				return json_decode($payload, true);

			default:
				throw new RemoteMethodInvocationException("Unsupported RMI encoding received: $encoding");
		}
	}

	protected function sendPayloadResponse(&$socket, $code, $data)
	{
		$encoding = 'PHP';
		$bytes = '';

		if (is_string($data) && strlen($data) < $this->maxPlainStringBytes) {
			$encoding = 'RAW';
			$bytes = $data;
		} else {
			$bytes = serialize($data);
		}

		$len = strlen($bytes);

		$this->connWriteLn($socket, $code . " PAYLOAD " . $encoding . " " . $len);

		if ($len) {
			$this->connWriteRaw($socket, $bytes);
		}
	}

	protected function connWriteRaw(&$stream, $bytes)
	{
		if (feof($stream) || feof($stream))
			return false;

		$info = stream_get_meta_data($stream);

		if ($info['timed_out'])
			return false;

		$written = 0;
		$length = strlen($bytes);

		while ($written < $length) {
			$result = fwrite($stream, substr($bytes, $written));

			if ($result === false)
				return false;

			$written += $result;
		}

		return $written;
	}

	protected function connReadExact(&$stream, $bytes)
	{
		$buff = '';
		$time = time();

		while (strlen($buff) < $bytes) {
			if (time() - $time > $this->readTimeout)
				return '__timeout';

			if (feof($stream) || !is_resource($stream))
				return false;

			$info = stream_get_meta_data($stream);

			if ($info['timed_out'])
				return '__timeout';

			$chunk = stream_get_contents($stream, $bytes - strlen($buff));

			if ($chunk === false)
				return false;

			if ($chunk === '') {
				$this->connCheckThrottle();
				continue;
			}

			$buff .= $chunk;
		}

		return $buff;
	}


	protected function connWriteLn(&$stream, $line)
	{
		if (feof($stream) || feof($stream)) {
			//syslog($errorSeverity, "SimpleRMIServer::connWriteLn(): Exiting worker " .  getmypid() . ": socket unavailable");
			return false;//$this->proc->join();
		}

		$info = stream_get_meta_data($stream);

		if ($info['timed_out']) {
			syslog(LOG_DEBUG, "SimpleRMIServer::connWriteLn(): Exiting worker " .  getmypid() . ": socket native timeout");
			return false;//$this->proc->join();
		}

		return fwrite($stream, "$line\n");
	}

	protected function connReadLn(&$stream)
	{
		$buff = '';
		$result = 0;
		$time = time();

		while(strpos($buff, "\n") === false) {
			if (time() - $time > $this->readTimeout)
				return '__timeout';	

			if (feof($stream)) {
				return false;//$this->proc->join();
			}

			if (!is_resource($stream)) {
				//syslog(LOG_DEBUG, "SimpleRMIServer::connReadLn(): Exiting worker " .  getmypid() . ": socket unavailable");
				return false;//$this->proc->join();
			}

			$info = stream_get_meta_data($stream);

			if ($info['timed_out']) {
				syslog(LOG_DEBUG, "SimpleRMIServer::connWriteLn(): Exiting worker " .  getmypid() . ": socket native timeout");
				return '__timeout';//$this->proc->join();
			}

			$this->connCheckThrottle();

			$result = stream_get_contents($stream, 1);

			if ($result === false || !is_resource($stream) || feof($stream))
				return false;

			$buff .= $result;

			if (strlen($buff) >= $this->maxLineLength)
				return '__overflow';
		}

		return trim($buff);
	}

        protected function encodePayload($payload)
        {
                // If payload is simple string, don't serialize
                if (is_string($payload) && strlen($payload) < $this->maxPlainStringBytes)
                        return base64_encode($payload);

                return 'PHP:' . base64_encode(serialize($payload));
        }

	protected function decodePayload($rawPayload)
	{
		list($encoding, $payload) = (strpos($rawPayload, ':'))
			? explode(':', $rawPayload, 2)
			: ['PLAIN', $rawPayload];	// default encoding is plaintext (base64)

		switch($encoding) {
			case 'PHP':
				return unserialize(base64_decode($payload));

			case 'JSON':
				return json_decode(base64_decode($payload));

			case 'PLAIN':	// default action
				if (($data = base64_decode($payload, true)) === false)
					return $payload;	// non-encoded plaintext fallback
				else
					return $data;

			default:
				throw new RemoteMethodInvocationException("Unsupported RMI encoding received: $encType");
		}		
	}

	protected function decodeMethodArgs($payload)
	{
		if (is_array($data = $this->decodePayload($payload)))
			return $data;

		return [$data];
	}

	protected function openServerSocket()
	{
		// seems to be enabled in php7 only
		$context = stream_context_create(array(
			'socket' => array(
				'so_reuseport' => true
				)
			));
		//stream_context_set_option($context, 'socket', 'so_reuseport', true);

		$this->serverSocket = stream_socket_server(
			'tcp://' . $this->host . ':' . $this->port, 
			$errno, 
			$errstr,
			STREAM_SERVER_BIND|STREAM_SERVER_LISTEN,
			$context
			);

		if (!$this->serverSocket)
		  throw new \Exception("$errstr ($errno)");

		// non-blocking socket to be able to handle child signals
		stream_set_blocking($this->serverSocket, false);
	}

	protected function closeServerSocket()
	{
		if (is_resource($this->serverSocket)) {
			$this->log("Closing server socket");
			stream_socket_shutdown($this->serverSocket, STREAM_SHUT_RDWR);
			fclose($this->serverSocket);
		}
	}

	protected function closeSocket($socket)
	{
		if (is_resource($socket)) {
			fclose($socket);
		}
	}

	protected function getCrashlog()
	{
		return str_replace('{contextName}', $this->contextName, $this->crashlog);
	}

	protected function handleCrash()
	{
		if ($error = error_get_last())
			$msg = "Unexpected shutdown on error: \n"
				. $error['type'] . ': ' . $error['message'] . ' in '
				. $error['file'] . ' on line ' . $error['line'] . "\n";
		else
			$msg = "Unexpected shutdown\n";

		$this->log($msg);
		$fp = fopen($this->getCrashlog(), 'a');
		fwrite($fp, $msg);
		fclose($fp);
	}

	protected function hasForkSupport()
	{
		return (function_exists('pcntl_fork') && !$this->disableFork);
	}

	protected function log($msg)
	{
		echo "$msg\n";
	}

	protected function registerShutdownHandler()
	{
		register_shutdown_function(
			array($this, 'handleProccessShutdown'));
	}

	protected function connCheckThrottle()
	{
		if ($this->throttleTimeout === false || !$this->connStarted || !$this->hasForkSupport() || $this->proc->isParent())
			return;

		$maxDuration = 0.5;

		if (time() - $this->connStarted > $this->throttleTimeout) {
			// Slowly increase up to maximum limit
			if ($this->throttleDuration * 2 > $maxDuration)
				$this->throttleDuration = $maxDuration;

			elseif ($this->throttleDuration < $maxDuration)
				$this->throttleDuration *= 2;

			usleep($this->throttleDuration * 1000000);
			$this->log('Throttling ' . $this->throttleDuration . 's');
		}
	}

	protected function forkChildWatchdog($pid)
	{
		$pidWatchdog = $this->proc->fork();

		if ($pidWatchdog) {
			$this->log("Forked watchdog with PID $pidWatchdog for child with PID $pid");
			return $pidWatchdog;
		}

		for ($i = 0; $i < $this->childTimeout; $i++) {
			sleep(1);

			// check if child is alive eg. receives signals
			if (!posix_kill($pid, 0)) {	
				$this->log("Shutting watchdog procces for child $pid");
				$this->doShutdown = true;
				return $this->proc->join();
			}

			// throttle niceness if proccess runs too long
			if ($this->childThrottle && isset($this->childNicenessThrottle[$i])) {
				$nice = $this->childNicenessThrottle[$i];

				$this->log("Setting niceness for child $pid to $nice");
				
				try {
					@system("renice $nice -p $pid");
				} catch(\Throwable $e) {
					$this->log("Failed to change niceness of child $pid to $nice: " . $e->getMessage());
				}
			}

			// change IO niceness if child runs over 30 seconds
			if ($this->childThrottle && isset($this->childIoNicenessThrottle[$i])) {
				$ionice = $this->childIoNicenessThrottle[$i];

				$this->log("Setting I/O niceness for child $pid to $ionice");
				
				try {
					@system("ionice -p $pid -c $ionice");
					@system("ionice -p $pid");
				} catch(\Throwable $e) {
					$this->log("Failed to change I/O niceness of child $pid to $ionice: " . $e->getMessage());
				}
			}
		}

		// Timed out, need to kill the child
		$this->log("Terminating unresponsive child $pid");
		syslog(LOG_CRIT, "SimpleRMIServer: Detected unresponsive child $pid");
		posix_kill($pid, SIGKILL);
		$this->doShutdown = true;
		return $this->proc->join();
	}

	protected function checkStream(&$socket)
	{

	}
}
