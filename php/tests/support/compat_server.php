<?php
require_once __DIR__ . '/extension_helpers.php';
require_once __DIR__ . '/compat_classes.php';

use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\CloudRMIServer;
use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\NativeRMIServer;
use ProfiWeb\Cloud\Tests\Compat\CompatTestHandler;
use ProfiWeb\Cloud\Tests\Compat\CompatTestInterface;

$options = getopt('', ['mode:', 'port:', 'host::', 'token::']);
$mode = $options['mode'] ?? 'extension';
$port = isset($options['port']) ? (int) $options['port'] : 12000;
$host = $options['host'] ?? '127.0.0.1';
$token = $options['token'] ?? 'compat-token';

if ($mode !== 'extension' && $mode !== 'native') {
    fwrite(STDERR, "Unknown mode {$mode}, expected extension or native.\n");
    exit(1);
}

if ($mode === 'extension') {
    ensure_cloudrmi_extension_loaded();

    class CompatCloudServer extends CloudRMIServer
    {
        public function __construct(string $host, int $port, string $token)
        {
            parent::__construct();
            $this->host = $host;
            $this->port = $port;
            $this->recoveryToken = $token;
            $this->disableFork = true;
            $this->enableAsyncCapability = false;
            $this->setMaxProtocolVersion(2);
            $this->setRequireAuth(true);
            $this->readTimeout = 5;
            $this->highReadTimeout = 5;
        }
    }

    $server = new CompatCloudServer($host, $port, $token);
} else {
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/ConnectionInterface.php';
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/NativeRMIServer.php';
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/SimpleRMIConnection.php';
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteException.php';
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteErrorException.php';
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteMethodInvocationException.php';
    require_once __DIR__ . '/../../class/System/ProcessControl/ProcessFork.php';

    class CompatNativeServer extends NativeRMIServer
    {
        public function __construct(string $host, int $port, string $token)
        {
            parent::__construct();
            $this->host = $host;
            $this->port = $port;
            $this->recoveryToken = $token;
            $this->disableFork = true;
            $this->readTimeout = 5;
            $this->highReadTimeout = 5;
        }
    }

    $server = new CompatNativeServer($host, $port, $token);
}

$handler = new CompatTestHandler();
$server->bind(CompatTestInterface::class, $handler);
$server->start();
