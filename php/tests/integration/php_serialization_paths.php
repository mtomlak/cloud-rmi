<?php
require_once __DIR__ . '/../support/test_harness.php';

use ProfiWeb\CloudRMI\CloudRMIClient;

function decode_inline_response(string $raw): mixed
{
	$payload = $raw;
	$encoding = 'PLAIN';

	if (strpos($raw, ':') !== false) {
		[$encoding, $payload] = explode(':', $raw, 2);
	}

	$encoding = strtoupper($encoding);

	switch ($encoding) {
		case 'PHP':
			return unserialize(base64_decode($payload));

		case 'JSON':
			return json_decode(base64_decode($payload), true);

		case 'RAW':
			return base64_decode($payload);

		case 'PLAIN':
			$decoded = base64_decode($payload, true);
			return ($decoded === false) ? $payload : $decoded;

		default:
			return $payload;
	}
}

function decode_payload_response(string $encoding, string $payload): mixed
{
	$encoding = strtoupper($encoding);

	switch ($encoding) {
		case 'PHP':
			return unserialize($payload);

		case 'JSON':
			return json_decode($payload, true);

		case 'RAW':
		case 'PLAIN':
			return $payload;

		default:
			return $payload;
	}
}

function read_response_body($stream, string $line): mixed
{
	$trimmed = trim($line);
	if (!preg_match('/^\d{3}\s+(.+)$/', $trimmed, $matches)) {
		throw new RuntimeException('malformed response line: ' . $trimmed);
	}

	$rest = $matches[1];
	if (preg_match('/^PAYLOAD\s+(\S+)\s+(\d+)$/i', $rest, $payloadMatches)) {
		$length = (int) $payloadMatches[2];
		$body = ($length > 0) ? stream_get_contents($stream, $length) : '';
		if ($body === false || strlen($body) !== $length) {
			throw new RuntimeException('payload response body truncated');
		}
		return decode_payload_response($payloadMatches[1], $body);
	}

	return decode_inline_response($rest);
}

function build_large_php_array(int $count, int $valueLength, string $char = 'V'): array
{
	$result = [];

	for ($i = 0; $i < $count; $i++) {
		$result[] = [
			'id' => $i,
			'value' => str_repeat($char, $valueLength),
		];
	}

	return $result;
}

cloudrmi_set_suite('php_serialization_paths');

$port = cloudrmi_random_port(19000, 400);
[$proc, $pipes, $port] = cloudrmi_start_php_server($port);
$exitCode = 0;

try {
	cloudrmi_wait_for_server('127.0.0.1', $port);
	$smallPayload = ['foo' => 'bar', 'nested' => ['n' => 1]];
	$largePayload = build_large_php_array(80, 12);

	cloudrmi_run_test('inline-php-mirror', function () use ($port, $smallPayload) {
		$client = new CloudRMIClient('127.0.0.1', $port);
		$client->connect();
		$smallResult = $client->call('TestRmiInterface', 'mirror', [$smallPayload]);
		$client->disconnect();
		if ($smallResult !== $smallPayload) {
			throw new RuntimeException('inline PHP mirror failed');
		}
	});

	cloudrmi_run_test('payload-php-mirror', function () use ($port, $largePayload) {
		$client = new CloudRMIClient('127.0.0.1', $port);
		$client->connect();
		$largeResult = $client->call(
			'TestRmiInterface',
			'mirror',
			[$largePayload],
			['payload' => true, 'encoding' => 'PHP']
		);
		$client->disconnect();
		if ($largeResult !== $largePayload) {
			throw new RuntimeException('payload PHP mirror failed');
		}
	});

	cloudrmi_run_test('payload-framing-large-result', function () use ($port) {
		$raw = stream_socket_client("tcp://127.0.0.1:{$port}");
		if ($raw === false) {
			throw new RuntimeException('unable to connect raw client');
		}
		fgets($raw, 2048);
		fwrite($raw, "PROTO 2\r\n");
		$protoLine = fgets($raw, 2048);
		if ($protoLine === false || strpos($protoLine, '200 PROTO 2') === false) {
			fclose($raw);
			throw new RuntimeException('proto negotiation failed for large result check');
		}

		$expectedLargeResult = build_large_php_array(120, 16, 'X');
		$argInline = 'PHP:' . base64_encode(serialize(120));
		fwrite($raw, "CALL TestRmiInterface largeResult {$argInline}\r\n");
		$responseLine = fgets($raw, 4096);
		if ($responseLine === false) {
			fclose($raw);
			throw new RuntimeException('large result response missing');
		}
		$trimmed = trim($responseLine);
		if (!preg_match('/^200\s+PAYLOAD\s+PHP\s+(\d+)$/i', $trimmed)) {
			fclose($raw);
			throw new RuntimeException('large result did not use payload framing: ' . $trimmed);
		}

		$body = read_response_body($raw, $responseLine);
		fclose($raw);
		if ($body !== $expectedLargeResult) {
			throw new RuntimeException('large result payload mismatch');
		}
	});
} catch (Throwable $e) {
	$exitCode = 1;
} finally {
	cloudrmi_stop_process($proc, $pipes);
	cloudrmi_print_timing_summary();
}

exit($exitCode);
