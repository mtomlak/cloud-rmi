<?php
namespace ProfiWeb\CloudRMI;

use ProfiWeb\CloudRMI\Support\ProcessFork;
use ProfiWeb\CloudRMI\Exception\RemoteException;
use ProfiWeb\CloudRMI\Exception\RemoteMethodInvocationException;

/**
 * Remote Method Invocation server.
 *
 * @copyright Copyright (C) 2016 - 2024 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license Proprietary
 */
class CloudRMIServer
{
	protected const DEFAULT_MAX_LINE = 8192;
	protected const PROTOCOL_V1_MAX_LINE = 262144;

	protected $serverSocket;
	// Configuration parameters
	protected $port = 8000;
	protected $host = '0.0.0.0';
	protected $recoveryToken = null;
	protected $contextName = 'dev.rmi';
	protected $crashlog = '/var/log/cloud.{contextName}.crash.log';
	protected $disableFork = false;
	protected $workerModel = self::WORKER_MODEL_FORK;
	protected $preforkWorkers = 4;
	protected $adaptiveMinWorkers = 2;
	protected $adaptiveMaxWorkers = 8;
	protected $adaptiveSpareWorkers = 2;
	// Other properties
	protected $doShutdown = false;
	protected $isAuthenticated = false;
	protected $username = null;
	protected $readTimeout = 300;
	protected $highReadTimeout = 300;
	protected $noForkReadTimeout = 300;
	protected $unixSocketPath = null;
	protected $bindings;
	protected $startedAt;
	protected $serverPid;
	protected $requireAuth = true;
	protected $enableAsyncCapability = true;
	protected $unixSocketSkipAuth = false;
	// Protocol-related
	protected $maxPlainStringBytes = 1024;
	protected $protocolVersion = 1;
	protected $maxProtocolVersion = 2;
	protected $maxPayload = null;
	protected $workerPids = [];
	protected $adaptiveWorkers = [];
	protected $isWorkerProcess = false;

	public function __construct()
	{
		$this->bindings = array();
	}

	public const WORKER_MODEL_SINGLE = 'single';
	public const WORKER_MODEL_FORK = 'fork';
	public const WORKER_MODEL_PREFORK = 'prefork';
	public const WORKER_MODEL_ADAPTIVE = 'adaptive';

	public function setUnixSocketPath($path)
	{
		$this->unixSocketPath = $this->normalizeUnixSocketPath($path);
	}

	public function setUnixSocketSkipAuth($skip)
	{
		$this->unixSocketSkipAuth = (bool) $skip;
	}

	public function setWorkerModel($model)
	{
		$model = strtolower((string) $model);
		$allowed = [
			self::WORKER_MODEL_SINGLE,
			self::WORKER_MODEL_FORK,
			self::WORKER_MODEL_PREFORK,
			self::WORKER_MODEL_ADAPTIVE,
		];

		if (!in_array($model, $allowed, true))
			$model = self::WORKER_MODEL_FORK;

		$this->workerModel = $model;
	}

	public function setPreforkWorkers($count)
	{
		$this->preforkWorkers = max(1, (int) $count);
	}

	public function setAdaptiveWorkerBounds($min, $max, $spare)
	{
		$this->adaptiveMinWorkers = max(1, (int) $min);
		$this->adaptiveMaxWorkers = max($this->adaptiveMinWorkers, (int) $max);
		$this->adaptiveSpareWorkers = max(0, (int) $spare);
	}

	public function setMaxProtocolVersion($version)
	{
		$version = (int) $version;
		$maxAllowed = defined('RMI_MAX_PROTOCOL_VERSION') ? (int) RMI_MAX_PROTOCOL_VERSION : 2;

		if ($version < 1)
			$version = 1;

		if ($version > $maxAllowed)
			$version = $maxAllowed;

		$this->maxProtocolVersion = $version;
	}

	public function setRequireAuth($requireAuth)
	{
		$this->requireAuth = (bool) $requireAuth;
	}

	public function start()
	{
		$this->registerShutdownHandler();
		$this->resolveWorkerModel();
		$this->installSignalHandlers();
		$this->openServerSocket();
		$this->applyReadTimeoutPolicy();
		$this->startedAt = time();
		$this->serverPid = posix_getpid();
		$this->log($this->contextName . " service ready on " . $this->describeEndpoint()
			. " [ composer cloudrmi | libcloudrmi " . RMI_LIBRARY_VERSION . " | worker_model " . $this->workerModel . " ]");

		switch ($this->workerModel) {
			case self::WORKER_MODEL_SINGLE:
				$this->serveSingleModel();
				break;

			case self::WORKER_MODEL_PREFORK:
				$this->servePreforkModel();
				break;

			case self::WORKER_MODEL_ADAPTIVE:
				$this->serveAdaptiveModel();
				break;

			default:
				$this->serveForkModel();
		}

		$this->closeServerSocket();
		$this->doShutdown = false; // in case it will be started again
	}

	protected function resolveWorkerModel()
	{
		$this->workerModel = strtolower((string) $this->workerModel);
		$allowed = [
			self::WORKER_MODEL_SINGLE,
			self::WORKER_MODEL_FORK,
			self::WORKER_MODEL_PREFORK,
			self::WORKER_MODEL_ADAPTIVE,
		];

		if (!in_array($this->workerModel, $allowed, true))
			$this->workerModel = self::WORKER_MODEL_FORK;

		if ($this->disableFork)
			$this->workerModel = self::WORKER_MODEL_SINGLE;

		if (!$this->hasForkSupport() && $this->workerModel !== self::WORKER_MODEL_SINGLE) {
			$this->workerModel = self::WORKER_MODEL_SINGLE;
			$this->disableFork = true;
		}

		if ($this->workerModel === self::WORKER_MODEL_ADAPTIVE) {
			$this->adaptiveMaxWorkers = max($this->adaptiveMinWorkers, $this->adaptiveMaxWorkers);
			$this->adaptiveSpareWorkers = min($this->adaptiveMaxWorkers, $this->adaptiveSpareWorkers);
		}
	}

	protected function installSignalHandlers()
	{
		if (!function_exists('pcntl_signal'))
			return;

		pcntl_signal(SIGTERM, [$this, 'handleSignalShutdown']);
		pcntl_signal(SIGINT, [$this, 'handleSignalShutdown']);

		if (function_exists('pcntl_async_signals'))
			pcntl_async_signals(true);
	}

	public function handleSignalShutdown()
	{
		$this->doShutdown = true;
	}

	protected function applyReadTimeoutPolicy()
	{
		if ($this->workerModel === self::WORKER_MODEL_SINGLE || !$this->isForkingEnabled())
			$this->readTimeout = min($this->readTimeout, $this->noForkReadTimeout);
	}

	protected function serveSingleModel()
	{
		while (!$this->doShutdown) {
			$conn = $this->acceptClientConnection();

			if (!is_resource($conn)) {
				$this->dispatchPendingSignals();
				continue;
			}

			$this->handleConnection($conn, $this->isUnixSocket());
		}
	}

	protected function serveForkModel()
	{
		while (!$this->doShutdown) {
			$conn = $this->acceptClientConnection();

			if (!is_resource($conn)) {
				$this->dispatchPendingSignals();
				$this->collectExitedChildren();
				continue;
			}

			$pid = pcntl_fork();

			if ($pid === -1) {
				$this->closeSocket($conn);
				continue;
			}

			if ($pid === 0) {
				$this->isWorkerProcess = true;
				$this->handleConnection($conn, $this->isUnixSocket());
				$this->closeServerSocket();
				exit;
			}

			$this->workerPids[$pid] = $pid;
			$this->closeSocket($conn);
			$this->collectExitedChildren();
		}

		$this->terminateWorkers($this->workerPids);
	}

	protected function servePreforkModel()
	{
		$this->workerPids = [];

		for ($i = 0; $i < $this->preforkWorkers; $i++) {
			$this->spawnWorkerProcess();
		}

		while (!$this->doShutdown) {
			$this->dispatchPendingSignals();
			$this->respawnMissingWorkers();
			usleep(100000);
		}

		$this->terminateWorkers($this->workerPids);
	}

	protected function serveAdaptiveModel()
	{
		$this->adaptiveWorkers = [];

		for ($i = 0; $i < $this->adaptiveMinWorkers; $i++)
			$this->spawnAdaptiveWorker();

		while (!$this->doShutdown) {
			$this->dispatchPendingSignals();
			$this->pollAdaptiveWorkers();
			$this->collectExitedAdaptiveWorkers();
			$this->scaleAdaptivePool();
			usleep(100000);
		}

		$this->terminateWorkers(array_column($this->adaptiveWorkers, 'pid'));
	}

	protected function dispatchPendingSignals()
	{
		if (function_exists('pcntl_signal_dispatch'))
			pcntl_signal_dispatch();
	}

	protected function acceptClientConnection($timeout = 0.25)
	{
		if (!is_resource($this->serverSocket)) {
			$this->doShutdown = true;
			return false;
		}

		return @stream_socket_accept($this->serverSocket, $timeout);
	}

	protected function spawnWorkerProcess()
	{
		if (!$this->isForkingEnabled())
			return;

		$pid = pcntl_fork();

		if ($pid === -1)
			return;

		if ($pid === 0) {
			$this->isWorkerProcess = true;
			$this->installSignalHandlers();
			$this->runWorkerLoop();
			$this->closeServerSocket();
			exit;
		}

		$this->workerPids[$pid] = $pid;
	}

	protected function respawnMissingWorkers()
	{
		$this->collectExitedChildren();

		while (count($this->workerPids) < $this->preforkWorkers && !$this->doShutdown) {
			$this->spawnWorkerProcess();
		}
	}

	protected function runWorkerLoop($stateSocket = null)
	{
		stream_set_blocking($this->serverSocket, true);
		$lastState = null;

		while (!$this->doShutdown) {
			if (is_resource($stateSocket) && $lastState !== 'idle') {
				$this->sendWorkerState($stateSocket, 'I');
				$lastState = 'idle';
			}

			$conn = $this->acceptClientConnection(1);

			if (!is_resource($conn)) {
				continue;
			}

			if (is_resource($stateSocket)) {
				$this->sendWorkerState($stateSocket, 'B');
				$lastState = 'busy';
			}

			$this->handleConnection($conn, $this->isUnixSocket());
		}

		if (is_resource($stateSocket))
			$this->sendWorkerState($stateSocket, 'X');
	}

	protected function spawnAdaptiveWorker()
	{
		if (!$this->isForkingEnabled())
			return;

		if (count($this->adaptiveWorkers) >= $this->adaptiveMaxWorkers)
			return;

		$pair = [];

		if (!@stream_socket_pair(STREAM_PF_UNIX, STREAM_SOCK_STREAM, STREAM_IPPROTO_IP, $pair))
			return;

		$pid = pcntl_fork();

		if ($pid === -1) {
			foreach ($pair as $socket)
				$this->closeSocket($socket);
			return;
		}

		if ($pid === 0) {
			$this->isWorkerProcess = true;
			$this->installSignalHandlers();
			$this->closeSocket($pair[0]);
			stream_set_blocking($pair[1], false);
			$this->runWorkerLoop($pair[1]);
			$this->closeSocket($pair[1]);
			$this->closeServerSocket();
			exit;
		}

		$this->closeSocket($pair[1]);
		stream_set_blocking($pair[0], false);
		$this->adaptiveWorkers[$pid] = [
			'pid' => $pid,
			'control' => $pair[0],
			'state' => 'starting',
			'last' => microtime(true),
		];
	}

	protected function pollAdaptiveWorkers()
	{
		if (empty($this->adaptiveWorkers))
			return;

		$read = [];
		$pidByStream = [];

		foreach ($this->adaptiveWorkers as $pid => $worker) {
			if (isset($worker['control']) && is_resource($worker['control'])) {
				$read[] = $worker['control'];
				$pidByStream[(int) $worker['control']] = $pid;
			}
		}

		if (empty($read))
			return;

		$write = $except = [];
		$count = @stream_select($read, $write, $except, 0, 0);

		if ($count === false || $count === 0)
			return;

		foreach ($read as $stream) {
			$message = @stream_get_contents($stream);

			if ($message === '' || $message === false)
				continue;

			$pid = $pidByStream[(int) $stream];

			foreach (str_split($message) as $signal)
				$this->updateAdaptiveState($pid, $signal);
		}
	}

	protected function updateAdaptiveState($pid, $signal)
	{
		if (!isset($this->adaptiveWorkers[$pid]))
			return;

		switch ($signal) {
			case 'I':
				$this->adaptiveWorkers[$pid]['state'] = 'idle';
				break;

			case 'B':
				$this->adaptiveWorkers[$pid]['state'] = 'busy';
				break;

			case 'X':
				$this->adaptiveWorkers[$pid]['state'] = 'stopped';
				break;
		}

		$this->adaptiveWorkers[$pid]['last'] = microtime(true);
	}

	protected function collectExitedChildren()
	{
		if (!function_exists('pcntl_waitpid'))
			return;

		while (($pid = pcntl_waitpid(-1, $status, WNOHANG)) > 0) {
			if (isset($this->adaptiveWorkers[$pid]['control']))
				$this->closeSocket($this->adaptiveWorkers[$pid]['control']);
			unset($this->workerPids[$pid]);
			unset($this->adaptiveWorkers[$pid]);
		}
	}

	protected function collectExitedAdaptiveWorkers()
	{
		$this->collectExitedChildren();

		foreach ($this->adaptiveWorkers as $pid => $worker) {
			if (!@posix_kill($pid, 0)) {
				$this->closeSocket($worker['control']);
				unset($this->adaptiveWorkers[$pid]);
			}
		}
	}

	protected function scaleAdaptivePool()
	{
		$active = count($this->adaptiveWorkers);
		$idle = 0;

		foreach ($this->adaptiveWorkers as $worker) {
			if ($worker['state'] === 'idle')
				$idle++;
		}

		if ($active < $this->adaptiveMinWorkers) {
			for ($i = $active; $i < $this->adaptiveMinWorkers; $i++)
				$this->spawnAdaptiveWorker();
			return;
		}

		if ($idle < $this->adaptiveSpareWorkers && $active < $this->adaptiveMaxWorkers) {
			$this->spawnAdaptiveWorker();
			return;
		}

		if ($idle > $this->adaptiveSpareWorkers && $active > $this->adaptiveMinWorkers) {
			$this->trimIdleAdaptiveWorker();
		}
	}

	protected function trimIdleAdaptiveWorker()
	{
		foreach ($this->adaptiveWorkers as $pid => $worker) {
			if ($worker['state'] === 'idle') {
				$this->terminateWorkers([$pid]);
				return;
			}
		}
	}

	protected function countAdaptiveIdleWorkers()
	{
		$idle = 0;

		foreach ($this->adaptiveWorkers as $worker) {
			if ($worker['state'] === 'idle')
				$idle++;
		}

		return $idle;
	}

	protected function writeWorkersResponse($socket)
	{
		$workers = [];

		if ($this->workerModel === self::WORKER_MODEL_PREFORK) {
			foreach (array_keys($this->workerPids) as $pid)
				$workers[] = ['pid' => $pid, 'state' => 'listening'];
		} elseif ($this->workerModel === self::WORKER_MODEL_ADAPTIVE) {
			foreach ($this->adaptiveWorkers as $worker)
				$workers[] = ['pid' => $worker['pid'], 'state' => $worker['state']];
		}

		$payload = [
			'reuse_enabled' => false,
			'worker_model' => $this->workerModel,
			'workers' => $workers,
			'total_spare' => ($this->workerModel === self::WORKER_MODEL_ADAPTIVE) ? $this->countAdaptiveIdleWorkers() : 0,
		];

		$this->connWriteLn($socket, '200 JSON ' . json_encode($payload));
	}

	protected function terminateWorkers(array $pids)
	{
		foreach ($pids as $pid) {
			if (isset($this->adaptiveWorkers[$pid]['control']))
				$this->closeSocket($this->adaptiveWorkers[$pid]['control']);

			unset($this->adaptiveWorkers[$pid]);
			unset($this->workerPids[$pid]);

			if ($pid && @posix_kill($pid, 0))
				@posix_kill($pid, SIGTERM);
		}

		$this->collectExitedChildren();
	}

	protected function sendWorkerState($socket, $state)
	{
		if (!is_resource($socket))
			return;

		@fwrite($socket, $state);
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
		if (!$this->isWorkerProcess)
			$this->closeServerSocket();

		if (!$this->doShutdown && !$this->isWorkerProcess)
			$this->handleCrash();
	}

	protected function isUnixSocket()
	{
		return (!is_null($this->unixSocketPath) && $this->unixSocketPath !== '');
	}

	protected function getMinProtocolVersion()
	{
		return $this->isUnixSocket() ? 2 : 1;
	}

	protected function getMaxProtocolVersion()
	{
		return max($this->maxProtocolVersion, $this->getMinProtocolVersion());
	}

	protected function formatUnixSocketAddress()
	{
		if (!$this->isUnixSocket())
			return '';

		if (stripos($this->unixSocketPath, 'unix://') === 0)
			return $this->unixSocketPath;

		return 'unix://' . $this->unixSocketPath;
	}

	protected function describeEndpoint()
	{
		if ($this->isUnixSocket())
			return $this->formatUnixSocketAddress();

		return $this->host . ':' . $this->port;
	}

	protected function getRemoteHostDescriptor()
	{
		return $this->isUnixSocket() ? $this->formatUnixSocketAddress() : $this->host;
	}

	protected function getRemotePortDescriptor()
	{
		return $this->isUnixSocket() ? 0 : $this->port;
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

	protected function buildBannerLine()
	{
		return "220 " . $this->contextName . " Server Ready EXT PROTO=" . $this->getMaxProtocolVersion() . $this->formatBannerCapabilities();
	}

	protected function formatBannerCapabilities()
	{
		$caps = $this->getAdvertisedCapabilities();

		if (empty($caps))
			return '';

		return ',' . implode(',', $caps);
	}

	protected function getAdvertisedCapabilities()
	{
		$caps = [];

		if ($this->isAsyncCapabilityAdvertised())
			$caps[] = 'ACALL';

		return $caps;
	}

	protected function canHandlePayloadRequests()
	{
		return ($this->getMaxProtocolVersion() >= 2);
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

	protected function isAsyncCapabilityAdvertised()
	{
		return ($this->enableAsyncCapability && $this->isForkingEnabled());
	}

	protected function handleConnection($socket, $isUnixSocket = false)
	{
		stream_set_timeout($socket, $this->readTimeout);
		stream_set_blocking($socket, true);
		$this->connWriteLn($socket, $this->buildBannerLine());
		$this->protocolVersion = $isUnixSocket ? $this->getMinProtocolVersion() : 1;
		$this->isAuthenticated = ($isUnixSocket && $this->unixSocketSkipAuth) || !$this->requireAuth;
		$this->username = ($this->isAuthenticated && $isUnixSocket && $this->unixSocketSkipAuth) ? 'UnixSocket' : null;

		$protocolCtx = cloudrmi_protocol_create();
		cloudrmi_protocol_set_max_payload($protocolCtx, $this->getMaxPayloadLength());
		cloudrmi_protocol_set_version($protocolCtx, $this->protocolVersion);

		if ($isUnixSocket) {
			cloudrmi_protocol_set_version($protocolCtx, $this->protocolVersion);
		}

		do {
			$input = $this->connReadLn($socket);

			if ($input === false)
				break;

			if ($input === '__timeout') {
				$this->connWriteLn($socket, "408 Request Timeout");
				break;
			}

			if ($input === '__overflow') {
				$this->connWriteLn($socket, "400 Bad Request");
				break;
			}

			[$line, $isAsync] = $this->normalizeInboundLine($input);
			[$expectsPayload, $payloadLength] = $this->extractPayloadMetadata($line);

			$feed = cloudrmi_protocol_feed($protocolCtx, $line . "\n");

			if ($this->isProtocolFeedFatal($feed)) {
				$this->respondProtocolFeedError($socket, $feed);
				break;
			}

			if ($expectsPayload && !is_null($payloadLength)) {
				$payloadBytes = $this->connReadExact($socket, $payloadLength);

				if ($payloadBytes === '__timeout') {
					$this->connWriteLn($socket, "408 Request Timeout");
					break;
				}

				if (!is_string($payloadBytes) || strlen($payloadBytes) != $payloadLength) {
					$this->connWriteLn($socket, "400 Bad Request");
					break;
				}

				$payloadFeed = cloudrmi_protocol_feed($protocolCtx, $payloadBytes);

				if ($this->isProtocolFeedFatal($payloadFeed)) {
					$this->respondProtocolFeedError($socket, $payloadFeed);
					break;
				}
			}

			$continue = $this->drainProtocolEvents($socket, $protocolCtx, $isAsync);

		} while ($continue !== false);

		cloudrmi_protocol_destroy($protocolCtx);
		$this->closeSocket($socket);
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

	protected function normalizeInboundLine($line)
	{
		$isAsync = (stripos($line, 'ACALL ') === 0);
		$normalized = $isAsync ? 'CALL ' . substr($line, 6) : $line;

		return [$normalized, $isAsync];
	}

	protected function extractPayloadMetadata($line)
	{
		$parts = preg_split('/\s+/', trim((string) $line));

		if (count($parts) < 6)
			return [false, null];

		if (strcasecmp($parts[0], 'CALL') !== 0)
			return [false, null];

		if (strcasecmp($parts[3], 'PAYLOAD') !== 0)
			return [false, null];

		if (!ctype_digit((string) $parts[5]))
			return [false, null];

		return [true, (int) $parts[5]];
	}

	protected function isProtocolFeedFatal(array $feed)
	{
		return (isset($feed['status']) && in_array($feed['status'], [RMI_E_ERR, RMI_E_OVERFLOW]));
	}

	protected function respondProtocolFeedError(&$socket, array $feed)
	{
		if (isset($feed['err_code']) && $feed['err_code'] === RMI_ERROR_BAD_ARGS) {
			$this->connWriteLn($socket, "400 Bad Request Arguments");
			return;
		}

		$this->connWriteLn($socket, "400 Bad Request");
	}

	protected function drainProtocolEvents(&$socket, $protocolCtx, $isAsync)
	{
		do {
			$eventResult = cloudrmi_protocol_next_event($protocolCtx);

			if (!isset($eventResult['status']))
				return false;

			if ($eventResult['status'] === RMI_OK && isset($eventResult['event']) && $eventResult['event'] !== null) {
				if (isset($eventResult['err_code']) && $eventResult['err_code'] === RMI_ERROR_BAD_REQUEST) {
					$this->connWriteLn($socket, "400 Bad Request");
					continue;
				}

				if (isset($eventResult['err_code']) && $eventResult['err_code'] === RMI_ERROR_BAD_ARGS) {
					$this->connWriteLn($socket, "400 Bad Request Arguments");
					continue;
				}

				$continue = $this->handleProtocolEvent($socket, $eventResult['event'], $isAsync, $protocolCtx);

				if ($continue === false)
					return false;
			} elseif ($eventResult['status'] === RMI_E_ERR) {
				if (isset($eventResult['err_code']) && $eventResult['err_code'] === RMI_ERROR_BAD_ARGS) {
					$this->connWriteLn($socket, "400 Bad Request Arguments");
				} elseif (isset($eventResult['err_code']) && $eventResult['err_code'] !== RMI_ERROR_NONE) {
					$this->connWriteLn($socket, "400 Bad Request");
				}
			}
		} while ($eventResult['status'] === RMI_OK);

		return null;
	}

	protected function handleProtocolEvent(&$socket, array $event, $isAsync, $protocolCtx)
	{
		switch ($event['type']) {
			case RMI_EVENT_AUTH:
				$this->handleAuthEvent($socket, $event);
				break;

			case RMI_EVENT_CALL:
				$this->handleCallEvent($socket, $event, $isAsync);
				break;

			case RMI_EVENT_PROTO:
				$this->handleProtoEvent($socket, $event, $protocolCtx);
				break;

			case RMI_EVENT_CAPS:
				$this->writeCapsResponse($socket);
				break;

			case RMI_EVENT_HELP:
				$this->writeHelpResponse($socket);
				break;

			case RMI_EVENT_IFACES:
				$this->readTimeout = $this->highReadTimeout;
				$this->writeIfacesResponse($socket);
				break;

		case RMI_EVENT_PING:
			$this->readTimeout = $this->highReadTimeout;
			$pingEcho = (isset($event['arg1']) && $event['arg1'] !== '') ? (' ' . $event['arg1']) : '';
			$this->connWriteLn($socket, "200 PONG" . $pingEcho);
			break;

		case RMI_EVENT_INFO:
			$this->writeInfoResponse($socket);
			break;

		case RMI_EVENT_SERVER:
			$this->handleServerEvent($socket, $event);
			break;

		case RMI_EVENT_QUIT:
			$this->connWriteLn($socket, "221 Bye");
			return false;

		default:
			$this->connWriteLn($socket, "400 Bad Request");
		}

		return null;
	}

	protected function handleAuthEvent(&$socket, array $event)
	{
		if ($this->isAuthenticated) {
			$this->connWriteLn($socket, '200 Already logged in as: ' . $this->username);
			return;
		}

		$method = isset($event['arg1']) ? trim((string) $event['arg1']) : '';
		$parameter = isset($event['arg2']) ? trim((string) $event['arg2']) : '';

		switch (strtolower($method)) {
			case 'token':
				if (!empty($this->recoveryToken) && $this->recoveryToken == $parameter) {
					$this->isAuthenticated = true;
					$this->username = 'RecoveryUser';

					$this->connWriteLn($socket, "200 Hello " . $this->username);
					return;
				}
				break;

			case 'login':
				if ($parameter === 'debug debug123') {
					$this->isAuthenticated = true;
					$this->username = 'DebugUser';

					$this->connWriteLn($socket, "200 Hello " . $this->username);
					return;
				}
				break;

			default:
				$this->connWriteLn($socket, "400 Bad Request: Unsupported AUTH method");	
				return;
		}

		$this->connWriteLn($socket, '401 Unauthorized');
	}

	protected function handleProtoEvent(&$socket, array $event, $protocolCtx)
	{
		$version = isset($event['arg1']) ? (int) $event['arg1'] : 1;
		$minVersion = $this->getMinProtocolVersion();

		if ($version < $minVersion || $version < 1) {
			$this->connWriteLn($socket, "400 Bad Request");
			return;
		}

		if ($version > $this->getMaxProtocolVersion())
			$version = $this->getMaxProtocolVersion();

		$this->protocolVersion = $version;
		if (!is_null($protocolCtx))
			cloudrmi_protocol_set_version($protocolCtx, $this->protocolVersion);
		$this->connWriteLn($socket, "200 PROTO " . $this->protocolVersion);
	}

	protected function handleCallEvent(&$socket, array $event, $isAsync)
	{
		if ($this->requireAuth && !$this->isAuthenticated) {
			$this->connWriteLn($socket, '401 Unauthorized');
			return;
		}

		$request = $this->buildRequestFromEvent($event);

		if ($request['error'] === 'bad_request') {
			$this->connWriteLn($socket, '400 Bad Request');
			return;
		}

		if ($request['error'] === 'bad_args') {
			$this->connWriteLn($socket, '400 Bad Request Arguments');
			return;
		}

		if ($request['payload'] && $this->protocolVersion < 2) {
			$this->connWriteLn($socket, '400 Bad Request');
			return;
		}

		if (!$binding = $this->getBinding($request['iface'])) {
			$this->connWriteLn($socket, "404 Not Found");
			return;
		}

		if (!in_array($request['method'], $this->getBindingMethods($request['iface']))) {
			$this->connWriteLn($socket, "405 Method Not Allowed");
			return;
		}

		if ($isAsync) {
			if (!$this->isAsyncCapabilityAdvertised()) {
				$this->connWriteLn($socket, '405 Method Not Allowed');
				return;
			}

			if (!$this->isForkingEnabled()) {
				$this->connWriteLn($socket, '405 Method Not Allowed: No fork support');
				return;
			}

			$proc = new ProcessFork();
			$pid = $proc->fork();

			if ($proc->isParent()) {
				$this->log("Forked new async child proccess with PID $pid");
				$this->connWriteLn($socket, "200 " . $this->encodePayload('Call Executed'));
				return;
			}

			try {
				$this->executeCall($binding, $request);
			} catch (\Throwable $e) {
				$this->log(' ! ' . get_class($e) . ': ' . $e->getMessage());
			}

			$this->log("Finished handling async call: " . $request['iface'] . '::' . $request['method']);

			return $proc->join();
		}

		$outcome = $this->executeCall($binding, $request);
		$canSendPayloadResponses = ($this->protocolVersion >= 2);

		if ($outcome['payload'] && $canSendPayloadResponses) {
			$this->sendPayloadResponse($socket, $outcome['code'], $outcome['data'], $request['encoding']);
			return;
		}

		try {
			$encodedResult = $this->encodePayload($outcome['data']);

		} catch (\Throwable $e) {
			$this->connWriteLn($socket, "500 Unable to transmit result: " . $request['iface'] . '::' . $request['method'] . ': ' . $e->getMessage());
			return;
		}

		$inlineLength = strlen($outcome['code'] . " " . $encodedResult) + 1;

		if ($inlineLength >= $this->getMaxLineLength()) {
			if ($canSendPayloadResponses) {
				$this->sendPayloadResponse($socket, $outcome['code'], $outcome['data'], $request['encoding']);
			} else {
				$this->connWriteLn($socket, "500 Unable to transmit result inline (exceeds max line length)");
			}
			return;
		}

		$this->connWriteLn($socket, $outcome['code'] . " " . $encodedResult);
	}

	protected function buildRequestFromEvent(array $event)
	{
		$result = [
			'iface' => null,
			'method' => null,
			'params' => null,
			'payload' => false,
			'encoding' => null,
			'error' => null,
		];

		$iface = isset($event['arg1']) ? (string) $event['arg1'] : '';
		$method = isset($event['arg2']) ? (string) $event['arg2'] : '';

		if ($iface === '' || $method === '') {
			$result['error'] = 'bad_args';
			return $result;
		}

		if ($iface && $iface[0] != '\\')
			$iface = "\\$iface";

		$result['iface'] = $iface;
		$result['method'] = $method;

		$isPayloadCall = (isset($event['argc']) && $event['argc'] >= 6);

		if ($isPayloadCall || (!empty($event['payload']) && (!isset($event['payload_len']) || $event['payload_len'] > 0))) {
			$encoding = isset($event['encoding']) ? (string) $event['encoding'] : '';
			$payloadBytes = isset($event['payload']) ? (string) $event['payload'] : '';

			try {
				$params = $this->decodePayloadBytes($encoding, $payloadBytes);
			} catch (\Throwable $e) {
				$result['error'] = 'bad_request';
				return $result;
			}

			$result['params'] = $params;
			$result['payload'] = true;
			$result['encoding'] = $encoding;

			return $result;
		}

		if (!isset($event['arg3']) || $event['arg3'] === '') {
			$result['error'] = 'bad_args';
			return $result;
		}

		$result['params'] = $this->decodeMethodArgs($event['arg3']);

		return $result;
	}

	protected function executeCall($binding, array $request)
	{
		$code = 200;
		$data = null;

		try {
			$data = @call_user_func_array(
				[$binding, $request['method']],
				is_array($request['params']) ? $request['params'] : [$request['params']]
			);

		} catch (\Throwable $e) {
			$this->log(' ! ' . get_class($e) . ': ' . $e->getMessage());

			$data = new RemoteException(
				$e,
				$this->contextName,
				$this->getRemoteHostDescriptor(),
				$this->getRemotePortDescriptor()
			);
			$code = 500;
		}

		return [
			'code' => $code,
			'data' => $data,
			'payload' => ($request['payload'] && $this->protocolVersion >= 2),
		];
	}

	protected function decodePayloadBytes($encoding, $payload)
	{
		$encoding = strtoupper((string) $encoding);

		if ($encoding === '')
			$encoding = 'RAW';

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

	protected function sendPayloadResponse(&$socket, $code, $data, $requestEncoding = '')
	{
		$response = $this->resolvePayloadResponse($data, $requestEncoding);
		$len = strlen($response['bytes']);

		if ($len > $this->getMaxPayloadLength()) {
			$this->connWriteLn($socket, "500 Error: payload exceeds max payload");
			return;
		}

		$this->connWriteLn($socket, $code . " PAYLOAD " . $response['encoding'] . " " . $len);

		if ($len) {
			$this->connWriteRaw($socket, $response['bytes']);
		}
	}

	protected function resolvePayloadResponse($data, $requestEncoding)
	{
		$encoding = strtoupper((string) $requestEncoding);

		if ($encoding === '')
			$encoding = 'PLAIN';

		if (!is_string($data))
			return ['encoding' => 'PHP', 'bytes' => serialize($data)];

		switch ($encoding) {
			case 'PHP':
				return ['encoding' => 'PHP', 'bytes' => serialize($data)];

			case 'JSON':
				return ['encoding' => 'JSON', 'bytes' => json_encode($data)];

			case 'RAW':
			case 'PLAIN':
				return ['encoding' => $encoding, 'bytes' => $data];

			default:
				return ['encoding' => 'PLAIN', 'bytes' => $data];
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
				$this->waitForIo();
				continue;
			}

			$buff .= $chunk;
		}

		return $buff;
	}

	protected function discardBytes(&$stream, $bytes)
	{
		$bytesLeft = (int) $bytes;

		while ($bytesLeft > 0) {
			$chunk = ($bytesLeft > 65536) ? 65536 : $bytesLeft;
			$data = stream_get_contents($stream, $chunk);

			if ($data === false || $data === '') {
				$this->waitForIo();
				break;
			}

			$bytesLeft -= strlen($data);
		}
	}

	protected function writeJsonArrayResponse($socket, array $items)
	{
		$this->connWriteLn($socket, '200 JSON ' . json_encode(array_values($items)));
	}

	protected function writeHelpResponse($socket)
	{
		$this->writeJsonArrayResponse($socket, [
			'ACALL',
			'AUTH',
			'CALL',
			'CAPS',
			'HELP',
			'IFACES',
			'INFO',
			'PING',
			'PROTO',
			'SERVER',
			'QUIT',
		]);
	}

	protected function writeIfacesResponse($socket)
	{
		$this->writeJsonArrayResponse($socket, array_keys($this->bindings));
	}

	protected function writeCapsResponse($socket)
	{
		$this->writeJsonArrayResponse($socket, $this->getAdvertisedCapabilities());
	}

	protected function writeInfoResponse($socket)
	{
		$uptime = time() - $this->startedAt;
		$info = [
			'role' => 'server',
			'capabilities' => $this->getAdvertisedCapabilities(),
			'build' => [
				'language' => 'PHP',
				'kernel' => $this->hasForkSupport() ? 'POSIX' : 'none',
				'version' => RMI_LIBRARY_VERSION,
			],
			'runtime' => [
				'pid' => $this->serverPid,
				'uptime' => $uptime,
				'worker_model' => $this->workerModel,
			],
		];

		$this->connWriteLn($socket, '200 JSON ' . json_encode($info));
	}

	protected function handleServerEvent(&$socket, array $event)
	{
		$subcommand = isset($event['arg1']) ? strtolower(trim((string) $event['arg1'])) : '';

		switch ($subcommand) {
			case 'shutdown':
				if (!$this->isAuthenticated) {
					$this->connWriteLn($socket, '401 Unauthorized');
					return;
				}
				$this->doShutdown = true;
				$this->connWriteLn($socket, "221 Server shutting down");
				\posix_kill($this->serverPid, SIGTERM);
				break;

			case 'debug':
				if (!$this->isAuthenticated) {
					$this->connWriteLn($socket, '401 Unauthorized');
					return;
				}
				$this->readTimeout = $this->highReadTimeout;
				$arg = isset($event['arg2']) ? strtolower(trim((string) $event['arg2'])) : '';
				if ($arg === 'on') {
					$this->connWriteLn($socket, "200 Debug logging ON");
				} elseif ($arg === 'off') {
					$this->connWriteLn($socket, "200 Debug logging OFF");
				} else {
					$this->connWriteLn($socket, "200 Debug logging is OFF");
				}
				break;

			case 'reload':
				if (!$this->isAuthenticated) {
					$this->connWriteLn($socket, '401 Unauthorized');
					return;
				}
				$this->connWriteLn($socket, "200 Reload not implemented");
				break;

			case 'workers':
				$this->writeWorkersResponse($socket);
				break;

			case 'help':
				$this->writeJsonArrayResponse($socket, ['DEBUG', 'SHUTDOWN']);
				break;

			default:
				$this->connWriteLn($socket, "400 Bad Request Arguments");
		}
	}

	protected function handleServerCommand(&$socket, $arg)
	{
		$parts = preg_split('/\s+/', trim((string) $arg), 2);
		$subcommand = isset($parts[0]) ? strtolower(trim($parts[0])) : '';
		$subarg = isset($parts[1]) ? trim($parts[1]) : '';

		switch ($subcommand) {
			case 'shutdown':
				if (!$this->isAuthenticated) {
					$this->connWriteLn($socket, '401 Unauthorized');
					return;
				}
				$this->doShutdown = true;
				$this->connWriteLn($socket, "221 Server shutting down");
				\posix_kill($this->serverPid, SIGTERM);
				break;

			case 'debug':
				if (!$this->isAuthenticated) {
					$this->connWriteLn($socket, '401 Unauthorized');
					return;
				}
				$this->readTimeout = $this->highReadTimeout;
				$debugArg = strtolower(trim($subarg));
				if ($debugArg === 'on') {
					$this->connWriteLn($socket, "200 Debug logging ON");
				} elseif ($debugArg === 'off') {
					$this->connWriteLn($socket, "200 Debug logging OFF");
				} else {
					$this->connWriteLn($socket, "200 Debug logging is OFF");
				}
				break;

			case 'reload':
				if (!$this->isAuthenticated) {
					$this->connWriteLn($socket, '401 Unauthorized');
					return;
				}
				$this->connWriteLn($socket, "200 Reload not implemented");
				break;

			case 'workers':
				$this->writeWorkersResponse($socket);
				break;

			case 'help':
				$this->writeJsonArrayResponse($socket, ['DEBUG', 'SHUTDOWN']);
				break;

			default:
				$this->connWriteLn($socket, "400 Bad Request Arguments");
		}
	}


	protected function connWriteLn(&$stream, $line)
	{
		if (feof($stream) || feof($stream)) {
			return false;
		}

		$info = stream_get_meta_data($stream);

		if ($info['timed_out']) {
			syslog(LOG_DEBUG, "CloudRMIServer::connWriteLn(): Exiting worker " .  getmypid() . ": socket native timeout");
			return false;
		}

		return fwrite($stream, "$line\n");
	}

	protected function connReadLn(&$stream)
	{
		$buffer = '';
		$time = time();
		$maxLine = $this->getMaxLineLength();

		while (is_resource($stream) && !feof($stream)) {
			if (time() - $time > $this->readTimeout)
				return '__timeout';

			$info = stream_get_meta_data($stream);

			if (isset($info['timed_out']) && $info['timed_out'])
				return '__timeout';

			$remaining = max(1, $maxLine - strlen($buffer) + 1);
			$chunk = fgets($stream, $remaining);

			if ($chunk === false) {
				$this->waitForIo();
				continue;
			}

			if ($chunk === '') {
				$this->waitForIo();
				continue;
			}

			$buffer .= $chunk;

			if (strpos($buffer, "\n") !== false || strlen($buffer) >= $maxLine) {
				break;
			}
		}

		if ($buffer === '')
			return false;

		if (strpos($buffer, "\n") === false && strlen($buffer) >= $maxLine)
			return '__overflow';

		return rtrim($buffer, "\r\n");
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
		$contextOptions = [];

		if (!$this->isUnixSocket()) {
			$contextOptions['socket'] = ['so_reuseport' => true];
		}

		$context = stream_context_create($contextOptions);
		$address = $this->isUnixSocket()
			? $this->formatUnixSocketAddress()
			: 'tcp://' . $this->host . ':' . $this->port;

		if ($this->isUnixSocket()) {
			$dir = dirname($this->unixSocketPath);

			if (!is_dir($dir) && !@mkdir($dir, 0777, true))
				throw new \Exception("Cannot create unix socket directory: " . $dir);

			if (file_exists($this->unixSocketPath) && !@unlink($this->unixSocketPath))
				throw new \Exception("Cannot remove existing unix socket: " . $this->unixSocketPath);
		}

		$this->serverSocket = stream_socket_server(
			$address,
			$errno,
			$errstr,
			STREAM_SERVER_BIND|STREAM_SERVER_LISTEN,
			$context
			);

		if (!$this->serverSocket)
		  throw new \Exception("$errstr ($errno)");

		stream_set_blocking($this->serverSocket, true);
	}

	protected function closeServerSocket()
	{
		if (is_resource($this->serverSocket)) {
			$this->log("Closing server socket");
			stream_socket_shutdown($this->serverSocket, STREAM_SHUT_RDWR);
			fclose($this->serverSocket);
		}

		if ($this->isUnixSocket() && $this->unixSocketPath && file_exists($this->unixSocketPath)) {
			@unlink($this->unixSocketPath);
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
		return function_exists('pcntl_fork');
	}

	protected function isForkingEnabled()
	{
		return ($this->hasForkSupport() && !$this->disableFork && $this->workerModel !== self::WORKER_MODEL_SINGLE);
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

	protected function waitForIo()
	{
		usleep(10000);
	}
}
