<?php
require_once __DIR__ . '/../support/test_harness.php';

use ProfiWeb\CloudRMI\CloudRMIClient;

cloudrmi_set_suite('mixed_sequences');

$exitCode = 0;
$phpProc = null;
$phpPipes = [];
$cProc = null;
$cPipes = [];

try {
	[$phpProc, $phpPipes, $phpPort] = cloudrmi_start_php_server(cloudrmi_random_port(18000, 400));
	cloudrmi_wait_for_server('127.0.0.1', $phpPort);

	cloudrmi_run_test('php-inline-payload-sequence', function () use ($phpPort) {
		$phpClient = new CloudRMIClient('127.0.0.1', $phpPort);
		$phpClient->connect();
		$first = $phpClient->call('TestRmiInterface', 'method', 'inline-first');
		$payload = $phpClient->call('TestRmiInterface', 'upload', 'payload-body', ['payload' => true, 'encoding' => 'RAW']);
		$third = $phpClient->call('TestRmiInterface', 'method', 'inline-third');

		if ($first !== 'OK' || $payload !== 'OK' || $third !== 'OK') {
			throw new RuntimeException('PHP server mixed sequence failed');
		}
	});

	cloudrmi_run_test('php-large-payload-expand', function () use ($phpPort) {
		$phpClient = new CloudRMIClient('127.0.0.1', $phpPort);
		$phpClient->connect();
		$largePayload = str_repeat('Chunk', 1600);
		$expanded = $phpClient->call('TestRmiInterface', 'expand', $largePayload, ['payload' => true, 'encoding' => 'RAW']);
		$phpClient->disconnect();
		$expectedExpanded = str_repeat($largePayload, 3);
		if ($expanded !== $expectedExpanded) {
			throw new RuntimeException('expanded payload response mismatch');
		}
	});

	cloudrmi_run_test('php-final-inline-after-payload', function () use ($phpPort) {
		$phpClient = new CloudRMIClient('127.0.0.1', $phpPort);
		$phpClient->connect();
		$finalInline = $phpClient->call('TestRmiInterface', 'method', 'inline-final');
		$phpClient->disconnect();
		if ($finalInline !== 'OK') {
			throw new RuntimeException('final inline call after payload failed');
		}
	});

	cloudrmi_stop_process($phpProc, $phpPipes);
	$phpProc = null;

	$cPort = cloudrmi_random_port(18500, 400);
	[$cProc, $cPipes] = cloudrmi_start_c_server($cPort, ['worker_model' => cloudrmi_worker_model()]);
	cloudrmi_wait_for_server('127.0.0.1', $cPort);

	cloudrmi_run_test('c-inline-echo', function () use ($cPort) {
		$cClient = new CloudRMIClient('127.0.0.1', $cPort);
		$cClient->connect();
		$cClient->authByToken('testtoken');

		$inline = $cClient->call('Test\\Integration\\EchoInterface', 'echo', 'c-inline');
		$cClient->disconnect();

		if ($inline !== 'TEST_ECHO_RESPONSE:c-inline') {
			throw new RuntimeException('C server inline call failed: ' . var_export($inline, true));
		}
	});

	cloudrmi_run_test('c-second-inline', function () use ($cPort) {
		$cClient = new CloudRMIClient('127.0.0.1', $cPort);
		$cClient->connect();
		$cClient->authByToken('testtoken');

		$again = $cClient->call('Test\\Integration\\EchoInterface', 'echo', 'c-inline-2');
		$cClient->disconnect();

		if ($again !== 'TEST_ECHO_RESPONSE:c-inline-2') {
			throw new RuntimeException('C server trailing inline failed: ' . var_export($again, true));
		}
	});

	cloudrmi_run_test('c-final-inline', function () use ($cPort) {
		$cClient = new CloudRMIClient('127.0.0.1', $cPort);
		$cClient->connect();
		$cClient->authByToken('testtoken');

		$finalEcho = $cClient->call('Test\\Integration\\EchoInterface', 'echo', 'c-inline-3');
		$cClient->disconnect();

		if ($finalEcho !== 'TEST_ECHO_RESPONSE:c-inline-3') {
			throw new RuntimeException('C server final inline after large payload failed');
		}
	});
} catch (Throwable $e) {
	$exitCode = 1;
} finally {
	if ($phpProc) {
		cloudrmi_stop_process($phpProc, $phpPipes);
	}

	if ($cProc) {
		cloudrmi_stop_process($cProc, $cPipes);
	}

	cloudrmi_print_timing_summary();
}

exit($exitCode);
