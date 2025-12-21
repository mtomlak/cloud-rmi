<?php
require_once __DIR__ . '/../bootstrap.php';

use ProfiWeb\CloudRMI\CloudRMIClient;
use ProfiWeb\CloudRMI\Exception\RemoteException;

function wait_for_server(string $host, int $port, int $retries = 20): void
{
	for ($i = 0; $i < $retries; $i++) {
		$sock = @stream_socket_client("tcp://{$host}:{$port}", $errno, $errstr, 0.2);
		if ($sock !== false) {
			fclose($sock);
			return;
		}
		usleep(100000);
	}
	throw new RuntimeException('server did not start');
}

function start_server(int $port): array
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
		1 => ['pipe', 'w'],
		2 => ['pipe', 'w'],
	];
	$proc = proc_open($cmd, $descriptors, $pipes);
	if (!is_resource($proc)) {
		throw new RuntimeException('failed to start server');
	}
	stream_set_blocking($pipes[1], false);
	stream_set_blocking($pipes[2], false);

	return [$proc, $pipes];
}

function stop_server($proc, array $pipes): void
{
	foreach ($pipes as $pipe) {
		fclose($pipe);
	}
	proc_terminate($proc);
	proc_close($proc);
}

$port = 19000 + random_int(0, 1000);
[$proc, $pipes] = start_server($port);
wait_for_server('127.0.0.1', $port);

try {
	$client = new CloudRMIClient('127.0.0.1', $port);
	$client->connect();

	$inlineThrown = false;
	try {
		$client->callRemoteMethod('TestRmiInterface', 'fail', 'first');
	} catch (RemoteException $e) {
		$inlineThrown = true;
		if (strpos($e->getMessage(), 'fail-inline:first') === false) {
			throw new RuntimeException('unexpected inline failure payload: ' . $e->getMessage());
		}
	}

	if (!$inlineThrown) {
		throw new RuntimeException('expected inline call to raise RemoteException');
	}

	$payloadThrown = false;
	try {
		$client->callRemoteMethod('TestRmiInterface', 'failPayload', 'payload-body', ['payload' => true]);
	} catch (RemoteException $e) {
		$payloadThrown = true;
		if (strpos($e->getMessage(), 'fail-payload:payload-body') === false) {
			throw new RuntimeException('unexpected payload failure payload: ' . $e->getMessage());
		}
	}

	if (!$payloadThrown) {
		throw new RuntimeException('expected payload call to raise RemoteException');
	}

	$ok = $client->callRemoteMethod('TestRmiInterface', 'method', 'ok');
	if ($ok !== 'OK') {
		throw new RuntimeException('follow-up call failed after exception handling');
	}

	$client->disconnect();

	fwrite(STDOUT, "CloudRMIServer error wrapping tests passed\n");
} finally {
	stop_server($proc, $pipes);
}
