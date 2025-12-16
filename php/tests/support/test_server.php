<?php
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/SimpleRMIServer.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/SimpleRMIClient.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteMethodInvocationException.php';

use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\SimpleRMIServer;

interface TestRmiInterface
{
    public function method($arg);
    public function upload($payload);
}

class TestRmiHandler implements TestRmiInterface
{
    public function method($arg)
    {
        return 'OK';
    }

    public function upload($payload)
    {
        return 'OK';
    }
}

class TestRMIServer extends SimpleRMIServer
{
    public function __construct(string $host, int $port, int $maxProto)
    {
        parent::__construct();
        $this->host = $host;
        $this->port = $port;
        $this->disableFork = true;
        $this->setMaxProtocolVersion($maxProto);
        $this->setRequireAuth(false);
        $this->readTimeout = 5;
        $this->highReadTimeout = 5;
    }
}

$options = getopt('', ['port:', 'host::', 'proto-max::']);
$port = isset($options['port']) ? (int) $options['port'] : 15555;
$host = $options['host'] ?? '127.0.0.1';
$protoMax = isset($options['proto-max']) ? (int) $options['proto-max'] : 2;

$server = new TestRMIServer($host, $port, $protoMax);
$handler = new TestRmiHandler();
$server->bind(TestRmiInterface::class, $handler);
$server->start();
