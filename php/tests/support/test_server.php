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
	public function __construct(
		string $host,
		int $port,
		int $maxProto,
		string $workerModel,
		?string $unixSocketPath = null,
		?int $preforkWorkers = null,
		?array $adaptiveBounds = null
	) {
		parent::__construct();
		$this->host = $host;
		$this->port = $port;
		$this->disableFork = ($workerModel === self::WORKER_MODEL_SINGLE);
		$this->setWorkerModel($workerModel);
		$this->enableAsyncCapability = ($workerModel !== self::WORKER_MODEL_SINGLE);
		$this->setMaxProtocolVersion($maxProto);
		$this->setRequireAuth(false);
		$this->readTimeout = 5;
		$this->setUnixSocketPath($unixSocketPath);

		if ($preforkWorkers !== null) {
			$this->setPreforkWorkers($preforkWorkers);
		}

		if ($adaptiveBounds !== null) {
			[$min, $max, $spare] = $adaptiveBounds;
			$this->setAdaptiveWorkerBounds($min, $max, $spare);
		}
	}
}

$options = getopt('', [
	'port:',
	'host::',
	'proto-max::',
	'worker-model::',
	'enable-fork::',
	'unix-socket::',
	'prefork-workers::',
	'adaptive-min::',
	'adaptive-max::',
	'adaptive-spare::',
]);

$port = isset($options['port']) ? (int) $options['port'] : 15555;
$host = $options['host'] ?? '127.0.0.1';
$protoMax = isset($options['proto-max']) ? (int) $options['proto-max'] : 2;
$workerModel = strtolower((string) ($options['worker-model'] ?? getenv('CLOUDRMI_WORKER_MODEL') ?? 'single'));
$preforkWorkers = isset($options['prefork-workers']) ? (int) $options['prefork-workers'] : null;
$adaptiveBounds = [
	isset($options['adaptive-min']) ? (int) $options['adaptive-min'] : 1,
	isset($options['adaptive-max']) ? (int) $options['adaptive-max'] : 4,
	isset($options['adaptive-spare']) ? (int) $options['adaptive-spare'] : 1,
];
$unixSocketPath = $options['unix-socket'] ?? null;

if (isset($options['enable-fork']) && !array_key_exists('worker-model', $options)) {
	$workerModel = ($options['enable-fork'] !== '0') ? CloudRMIServer::WORKER_MODEL_FORK : CloudRMIServer::WORKER_MODEL_SINGLE;
}

$server = new TestRMIServer($host, $port, $protoMax, $workerModel, $unixSocketPath, $preforkWorkers, $adaptiveBounds);

$handler = new TestRmiHandler();
$server->bind(TestRmiInterface::class, $handler);
$server->start();
