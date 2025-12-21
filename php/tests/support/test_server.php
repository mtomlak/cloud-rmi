<?php
require_once __DIR__ . '/../bootstrap.php';

use ProfiWeb\CloudRMI\CloudRMIServer;

interface TestRmiInterface
{
    public function method($arg);
    public function upload($payload);
    public function expand($payload);
    public function mirror($payload);
    public function largeResult($count);
    public function fail($arg);
    public function failPayload($payload);
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

    public function expand($payload)
    {
        $body = (string) $payload;
        return str_repeat($body, 3);
    }

    public function mirror($payload)
    {
        return $payload;
    }

    public function largeResult($count)
    {
        $items = [];
        $limit = max(0, (int) $count);

        for ($i = 0; $i < $limit; $i++) {
            $items[] = [
                'id' => $i,
                'value' => str_repeat('X', 16),
            ];
        }

        return $items;
    }

    public function fail($arg)
    {
        throw new \RuntimeException('fail-inline:' . $arg);
    }

    public function failPayload($payload)
    {
        $label = is_scalar($payload) ? (string) $payload : gettype($payload);
        throw new \RuntimeException('fail-payload:' . $label);
    }
}

class TestRMIServer extends CloudRMIServer
{
	public function __construct(string $host, int $port, int $maxProto, bool $enableFork, ?string $unixSocketPath = null)
	{
		parent::__construct();
		$this->host = $host;
		$this->port = $port;
		$this->disableFork = !$enableFork;
		$this->enableAsyncCapability = $enableFork;
		$this->setMaxProtocolVersion($maxProto);
		$this->setRequireAuth(false);
		$this->readTimeout = 5;
		$this->highReadTimeout = 5;
		$this->setUnixSocketPath($unixSocketPath);
	}
}

$options = getopt('', ['port:', 'host::', 'proto-max::', 'enable-fork::', 'unix-socket::']);
$port = isset($options['port']) ? (int) $options['port'] : 15555;
$host = $options['host'] ?? '127.0.0.1';
$protoMax = isset($options['proto-max']) ? (int) $options['proto-max'] : 2;
$enableFork = isset($options['enable-fork']) && $options['enable-fork'] !== '0';
$unixSocketPath = $options['unix-socket'] ?? null;

$server = new TestRMIServer($host, $port, $protoMax, $enableFork, $unixSocketPath);

$handler = new TestRmiHandler();
$server->bind(TestRmiInterface::class, $handler);
$server->start();
