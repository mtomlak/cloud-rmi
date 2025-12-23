<?php
require_once __DIR__ . '/../bootstrap.php';

function ok($cond, $msg) {
    if (!$cond) {
        fwrite(STDERR, "FAIL: $msg\n");
        exit(1);
    }
}

function eq($a, $b, $msg) {
    if ($a !== $b) {
        fwrite(STDERR, "FAIL: $msg (expected "
            . var_export($a, true)
            . ", got "
            . var_export($b, true)
            . ")\n");
        exit(1);
    }
}

/* ---------- valid banners ---------- */

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

/* ---------- invalid banners ---------- */

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

fwrite(STDOUT, "Banner parse tests passed\n");
