<?php
require_once __DIR__ . '/../support/test_harness.php';

cloudrmi_set_suite('unit-banner-parse');

function ok($cond, $msg)
{
	if (!$cond) {
		throw new RuntimeException("FAIL: {$msg}");
	}
}

function eq($a, $b, $msg)
{
	if ($a !== $b) {
		throw new RuntimeException("FAIL: {$msg} (expected "
			. var_export($a, true)
			. ", got "
			. var_export($b, true)
			. ")");
	}
}

$exitCode = 0;

try {
	cloudrmi_run_test('valid-banners', function () {
		$tests = [
			"220 ctx Server Ready EXT PROTO=2" => [2, []],
			"220 ctx EXT PROTO=2,STATS"        => [2, ["STATS"]],
			"220 ctx EXT PROTO=1"              => [1, []],
			"220 ctx EXT STATS"                => [1, ["STATS"]],
			"220 ctx EXT SERVER-STATS"         => [1, ["SERVER-STATS"]],
			"220 ctx Server Ready"             => [1, []],
		];

		foreach ($tests as $line => [$ver, $caps]) {
			$res = cloudrmi_parse_banner($line);
			ok($res !== false, "parse: $line");
			eq($ver, $res['advertised_max_protocol'], "proto: $line");
			eq($caps, $res['capabilities'], "caps: $line");
		}
	});

	cloudrmi_run_test('invalid-banners', function () {
		$bad = [
			"220",
			"220ctx EXT PROTO=2",
			"220 ctx EXT PROTO=0",
			"220 ctx EXT PROTO=999",
			"220 ctx EXT PROTO=2,BAD!",
			"220 ctx EXT PROTO=",
		];

		foreach ($bad as $line) {
			$res = cloudrmi_parse_banner($line);
			ok($res === false, "reject: $line");
		}
	});
} catch (Throwable $e) {
	$exitCode = 1;
} finally {
	cloudrmi_print_timing_summary();
}

exit($exitCode);
