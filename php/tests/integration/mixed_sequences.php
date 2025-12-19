<?php
require_once __DIR__ . '/../bootstrap.php';

use ProfiWeb\CloudRMI\CloudRMIClient;

$rootDir = dirname(__DIR__, 3);

function wait_for_server(string $host, int $port, int $retries = 30): void
{
	for ($i = 0; $i < $retries; $i++) {
		$sock = @stream_socket_client("tcp://{$host}:{$port}", $errno, $errstr, 0.2);
		if ($sock !== false) {
			fclose($sock);
			return;
		}
		usleep(100000);
	}
	throw new RuntimeException("server on {$host}:{$port} did not start");
}

function start_php_server(int $port): array
{
	$cmd = cloudrmi_test_command(
		__DIR__ . '/../support/test_server.php',
		[
			'--host=127.0.0.1',
			'--port=' . $port,
			'--proto-max=2',
		]
	);
	$descriptors = [
		0 => ['pipe', 'r'],
		1 => ['file', '/dev/null', 'a'],
		2 => ['file', '/dev/null', 'a'],
	];
	$proc = proc_open($cmd, $descriptors, $pipes);
	if (!is_resource($proc)) {
		throw new RuntimeException('failed to start PHP test server');
	}
	return [$proc, $pipes];
}

function start_c_server(int $port): array
{
	global $rootDir;

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

function stop_process($proc, array $pipes): void
{
	foreach ($pipes as $pipe) {
		fclose($pipe);
	}

	proc_terminate($proc);

	$deadline = time() + 5;
	while (proc_get_status($proc)['running'] && time() < $deadline) {
		usleep(100000);
	}

	if (proc_get_status($proc)['running']) {
		@proc_terminate($proc, SIGKILL);
	}

	proc_close($proc);
}

$phpPort = 18000 + random_int(0, 400);
[$phpProc, $phpPipes] = start_php_server($phpPort);
wait_for_server('127.0.0.1', $phpPort);
fwrite(STDOUT, "PHP server ready on {$phpPort}\n");

try {
	$phpClient = new CloudRMIClient('127.0.0.1', $phpPort);
	$phpClient->connect();
	$first = $phpClient->call('TestRmiInterface', 'method', 'inline-first');
	$payload = $phpClient->call('TestRmiInterface', 'upload', 'payload-body', ['payload' => true, 'encoding' => 'RAW']);
	$third = $phpClient->call('TestRmiInterface', 'method', 'inline-third');

	if ($first !== 'OK' || $payload !== 'OK' || $third !== 'OK') {
		throw new RuntimeException('PHP server mixed sequence failed');
	}
	fwrite(STDOUT, "PHP server inline/payload sequence ok\n");

	$largePayload = str_repeat('Chunk', 1600); // 8000 bytes
	$expanded = $phpClient->call('TestRmiInterface', 'expand', $largePayload, ['payload' => true, 'encoding' => 'RAW']);
	$expectedExpanded = str_repeat($largePayload, 3);
	if ($expanded !== $expectedExpanded) {
		throw new RuntimeException('expanded payload response mismatch');
	}
	fwrite(STDOUT, "PHP server large payload ok\n");

	$finalInline = $phpClient->call('TestRmiInterface', 'method', 'inline-final');
	if ($finalInline !== 'OK') {
		throw new RuntimeException('final inline call after payload failed');
	}
	fwrite(STDOUT, "PHP server final inline ok\n");

	$phpClient->disconnect();
} finally {
	stop_process($phpProc, $phpPipes);
}

$cPort = 18500 + random_int(0, 400);
[$cProc, $cPipes] = start_c_server($cPort);
wait_for_server('127.0.0.1', $cPort);
fwrite(STDOUT, "C server ready on {$cPort}\n");

try {
	$cClient = new CloudRMIClient('127.0.0.1', $cPort);
	$cClient->connect();
	$cClient->authByToken('testtoken');

	$inline = $cClient->call('Test\\Integration\\EchoInterface', 'echo', 'c-inline');
	if ($inline !== 'TEST_ECHO_RESPONSE:c-inline') {
		throw new RuntimeException('C server inline call failed: ' . var_export($inline, true));
	}
	fwrite(STDOUT, "C server inline response ok\n");

	$again = $cClient->call('Test\\Integration\\EchoInterface', 'echo', 'c-inline-2');
	if ($again !== 'TEST_ECHO_RESPONSE:c-inline-2') {
		throw new RuntimeException('C server trailing inline failed: ' . var_export($again, true));
	}
	fwrite(STDOUT, "C server second inline ok\n");

	$finalEcho = $cClient->call('Test\\Integration\\EchoInterface', 'echo', 'c-inline-3');
	if ($finalEcho !== 'TEST_ECHO_RESPONSE:c-inline-3') {
		throw new RuntimeException('C server final inline after large payload failed');
	}
	fwrite(STDOUT, "C server final inline ok\n");

	$cClient->disconnect();
} finally {
	stop_process($cProc, $cPipes);
}

fwrite(STDOUT, "Mixed sequence integration tests passed\n");
