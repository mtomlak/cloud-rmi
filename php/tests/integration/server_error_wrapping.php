<?php
require_once __DIR__ . '/../support/test_harness.php';

use ProfiWeb\CloudRMI\CloudRMIClient;
use ProfiWeb\CloudRMI\Exception\RemoteException;

cloudrmi_set_suite('server_error_wrapping');

$port = cloudrmi_random_port(19000, 800);
[$proc, $pipes, $port] = cloudrmi_start_php_server($port);
$exitCode = 0;

try {
	cloudrmi_wait_for_server('127.0.0.1', $port);
	$client = new CloudRMIClient('127.0.0.1', $port);
	$client->connect();

	cloudrmi_run_test('inline-error-wrapped', function () use ($client) {
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
	});

	cloudrmi_run_test('payload-error-wrapped', function () use ($client) {
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
	});

	cloudrmi_run_test('recover-after-error', function () use ($client) {
		$ok = $client->callRemoteMethod('TestRmiInterface', 'method', 'ok');
		if ($ok !== 'OK') {
			throw new RuntimeException('follow-up call failed after exception handling');
		}
	});

	$client->disconnect();
} catch (Throwable $e) {
	$exitCode = 1;
} finally {
	cloudrmi_stop_process($proc, $pipes);
	cloudrmi_print_timing_summary();
}

exit($exitCode);
