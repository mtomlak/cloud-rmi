<?php
require_once __DIR__ . '/../bootstrap.php';

use ProfiWeb\CloudRMI\CloudRMIClient;

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

function start_test_server(int $port): array
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

function stop_server($proc, array $pipes): void
{
	foreach ($pipes as $pipe) {
		fclose($pipe);
	}

	proc_terminate($proc);
	proc_close($proc);
}

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
	if (!preg_match('/^\\d{3}\\s+(.+)$/', $trimmed, $matches)) {
		throw new RuntimeException('malformed response line: ' . $trimmed);
	}

	$rest = $matches[1];
	if (preg_match('/^PAYLOAD\\s+(\\S+)\\s+(\\d+)$/i', $rest, $payloadMatches)) {
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

$port = 19000 + random_int(0, 400);
[$proc, $pipes] = start_test_server($port);
wait_for_server('127.0.0.1', $port);

try {
	$client = new CloudRMIClient('127.0.0.1', $port);
	$client->connect();

	$smallPayload = ['foo' => 'bar', 'nested' => ['n' => 1]];
	$smallResult = $client->call('TestRmiInterface', 'mirror', [$smallPayload]);
	if ($smallResult !== $smallPayload) {
		throw new RuntimeException('inline PHP mirror failed');
	}

	$largePayload = build_large_php_array(80, 12); // comfortably above inline threshold
	$largeResult = $client->call(
		'TestRmiInterface',
		'mirror',
		[$largePayload],
		['payload' => true, 'encoding' => 'PHP']
	);
	if ($largeResult !== $largePayload) {
		throw new RuntimeException('payload PHP mirror failed');
	}

	$client->disconnect();

	$raw = stream_socket_client("tcp://127.0.0.1:{$port}");
	if ($raw === false) {
		throw new RuntimeException('unable to connect raw client');
	}
	fgets($raw, 2048); // banner
	fwrite($raw, "PROTO 2\r\n");
	$protoLine = fgets($raw, 2048);
	if ($protoLine === false || strpos($protoLine, '200 PROTO 2') === false) {
		throw new RuntimeException('proto negotiation failed for large result check');
	}

	$expectedLargeResult = build_large_php_array(120, 16, 'X');
	$argInline = 'PHP:' . base64_encode(serialize(120));
	fwrite($raw, "CALL TestRmiInterface largeResult {$argInline}\r\n");
	$responseLine = fgets($raw, 4096);
	if ($responseLine === false) {
		throw new RuntimeException('large result response missing');
	}
	$trimmed = trim($responseLine);
	if (!preg_match('/^200\\s+PAYLOAD\\s+PHP\\s+(\\d+)$/i', $trimmed)) {
		throw new RuntimeException('large result did not use payload framing: ' . $trimmed);
	}

	$body = read_response_body($raw, $responseLine);
	if ($body !== $expectedLargeResult) {
		throw new RuntimeException('large result payload mismatch');
	}
	fclose($raw);
} finally {
	stop_server($proc, $pipes);
}

fwrite(STDOUT, "PHP serialization integration tests passed\n");
