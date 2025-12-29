<?php
require_once __DIR__ . '/../bootstrap.php';

$GLOBALS['cloudrmi_suite_name'] = '';
$GLOBALS['cloudrmi_test_results'] = [];

function cloudrmi_set_suite(string $name): void
{
	$GLOBALS['cloudrmi_suite_name'] = $name;
}

function cloudrmi_worker_model(): string
{
	$model = getenv('CLOUDRMI_WORKER_MODEL');

	if ($model === false || $model === '') {
		return 'single';
	}

	return strtolower($model);
}

function cloudrmi_worker_prefix(): string
{
	$model = cloudrmi_worker_model();

	return ($model === 'single') ? '' : ($model . ':');
}

function cloudrmi_run_test(string $name, callable $fn): void
{
	$display = cloudrmi_worker_prefix() . $name;
	$start = microtime(true);

	echo "testing {$display}... ";

	try {
		$fn();
		$duration = microtime(true) - $start;
		echo "ok\n";
		$GLOBALS['cloudrmi_test_results'][] = [
			'name' => $name,
			'display' => $display,
			'duration' => $duration,
			'worker' => cloudrmi_worker_model(),
		];
	} catch (Throwable $e) {
		$duration = microtime(true) - $start;
		echo "FAIL\n";
		fwrite(STDERR, $e->getMessage() . "\n");
		$GLOBALS['cloudrmi_test_results'][] = [
			'name' => $name,
			'display' => $display,
			'duration' => $duration,
			'worker' => cloudrmi_worker_model(),
		];
		throw $e;
	}
}

function cloudrmi_print_timing_summary(): void
{
	$results = $GLOBALS['cloudrmi_test_results'];

	if (empty($results)) {
		return;
	}

	$suite = $GLOBALS['cloudrmi_suite_name'];
	$heading = "Timing summary";

	if ($suite !== '') {
		$heading .= " ({$suite})";
	}

	echo "\n{$heading}:\n";

	$byWorker = [];
	foreach ($results as $res) {
		$byWorker[$res['worker']][] = $res;
	}

	foreach ($byWorker as $worker => $entries) {
		$total = array_reduce($entries, static function ($carry, $item) {
			return $carry + ($item['duration'] ?? 0.0);
		}, 0.0);
		printf("- %s: %.3fs total\n", $worker, $total);
		foreach ($entries as $entry) {
			printf("  - %s: %.3fs\n", $entry['display'], $entry['duration']);
		}
	}
}

function cloudrmi_random_port(int $base = 14000, int $span = 1200): int
{
	return $base + random_int(0, $span);
}

function cloudrmi_wait_for_server(string $host, int $port, int $retries = 30, int $delayMicroseconds = 100000): void
{
	for ($i = 0; $i < $retries; $i++) {
		$sock = @stream_socket_client("tcp://{$host}:{$port}", $errno, $errstr, 0.2);
		if ($sock !== false) {
			fclose($sock);
			return;
		}
		usleep($delayMicroseconds);
	}

	throw new RuntimeException("server on {$host}:{$port} did not start");
}

function cloudrmi_wait_for_unix_socket(string $path, int $retries = 30, int $delayMicroseconds = 100000): void
{
	for ($i = 0; $i < $retries; $i++) {
		if (@file_exists($path)) {
			$sock = @stream_socket_client("unix://{$path}", $errno, $errstr, 0.2);
			if ($sock !== false) {
				fclose($sock);
				return;
			}
		}
		usleep($delayMicroseconds);
	}

	throw new RuntimeException("unix socket {$path} did not start");
}

function cloudrmi_start_php_server(?int $port = null, array $options = []): array
{
	$port = $port ?? cloudrmi_random_port();
	$host = $options['host'] ?? '127.0.0.1';
	$workerModel = $options['worker_model'] ?? cloudrmi_worker_model();
	$args = [
		'--host=' . $host,
		'--port=' . $port,
		'--proto-max=' . ($options['proto_max'] ?? 2),
		'--worker-model=' . $workerModel,
		'--prefork-workers=' . ($options['prefork_workers'] ?? getenv('CLOUDRMI_PREFORK_WORKERS') ?: 2),
		'--adaptive-min=' . ($options['adaptive_min'] ?? getenv('CLOUDRMI_ADAPTIVE_MIN') ?: 1),
		'--adaptive-max=' . ($options['adaptive_max'] ?? getenv('CLOUDRMI_ADAPTIVE_MAX') ?: 4),
		'--adaptive-spare=' . ($options['adaptive_spare'] ?? getenv('CLOUDRMI_ADAPTIVE_SPARE') ?: 1),
	];

	if (!empty($options['unix_socket'])) {
		$args[] = '--unix-socket=' . $options['unix_socket'];
	}

	$cmd = cloudrmi_test_command(__DIR__ . '/test_server.php', $args);
	$descriptors = [
		0 => ['pipe', 'r'],
		1 => ['file', '/dev/null', 'a'],
		2 => ['file', '/dev/null', 'a'],
	];
	$proc = proc_open($cmd, $descriptors, $pipes);
	if (!is_resource($proc)) {
		throw new RuntimeException('failed to start PHP test server');
	}

	return [$proc, $pipes, $port];
}

function cloudrmi_start_c_server(int $port, array $options = []): array
{
	$rootDir = dirname(__DIR__, 3);
	$binaryPath = $rootDir . '/rmi-server';
	if (!file_exists($binaryPath)) {
		throw new RuntimeException('rmi-server binary not found; build it via make');
	}
	$binary = realpath($binaryPath) ?: $binaryPath;

	$configPath = $rootDir . '/tests/integration/fixtures/rmi-server.conf';
	$mapPath = $rootDir . '/tests/integration/fixtures/interfaces.map';
	$scriptsPath = $rootDir . '/tests/integration/fixtures/scripts';
	$config = realpath($configPath) ?: $configPath;
	$map = realpath($mapPath) ?: $mapPath;
	$scripts = realpath($scriptsPath) ?: $scriptsPath;

	if ($config === false || $map === false || $scripts === false) {
		throw new RuntimeException('missing integration fixtures for C server');
	}

	$cmd = [
		$binary,
		'-c',
		$config,
		'-m',
		$map,
		'-p',
		(string) $port,
		'-e',
		$scripts,
		'-b',
		'Test\\Integration',
		'-t',
		'testtoken',
	];

	if (!empty($options['worker_model'])) {
		$cmd[] = '-w';
		$cmd[] = $options['worker_model'];
	}

	$descriptors = [
		0 => ['pipe', 'r'],
		1 => ['file', '/dev/null', 'a'],
		2 => ['file', '/dev/null', 'a'],
	];

	$proc = proc_open($cmd, $descriptors, $pipes, $rootDir);
	if (!is_resource($proc)) {
		throw new RuntimeException('failed to start rmi-server binary');
	}

	return [$proc, $pipes];
}

function cloudrmi_stop_process($proc, array $pipes): void
{
	foreach ($pipes as $pipe) {
		if (is_resource($pipe)) {
			fclose($pipe);
		}
	}

	if (!is_resource($proc)) {
		return;
	}

	proc_terminate($proc);
	$deadline = microtime(true) + 5.0;
	$status = proc_get_status($proc);

	while ($status['running'] && microtime(true) < $deadline) {
		usleep(100000);
		$status = proc_get_status($proc);
	}

	if ($status['running']) {
		@proc_terminate($proc, SIGKILL);
	}

	proc_close($proc);
}
