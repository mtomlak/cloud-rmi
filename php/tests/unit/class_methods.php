<?php
declare(strict_types=1);

use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\CloudRMIClient;
use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\CloudRMIServer;

function assert_true($condition, string $message): void
{
    if (!$condition) {
        fwrite(STDERR, "Assertion failed: {$message}\n");
        exit(1);
    }
}

function assert_methods(array $expected, string $class): void
{
    $methods = array_map('strtolower', get_class_methods($class));
    foreach ($expected as $method) {
        assert_true(in_array(strtolower($method), $methods, true), "{$class} missing method {$method}");
    }
}

$expectedClient = [
    '__construct',
    'connect',
    'disconnect',
    'isConnected',
    'authByToken',
    'callRemoteMethod',
    'call',
    'setConnection',
    'getConnection',
];

$expectedServer = [
    '__construct',
    'setUnixSocketPath',
    'setUnixSocketSkipAuth',
    'setMaxProtocolVersion',
    'setRequireAuth',
    'bind',
    'handleProccessShutdown',
    'start',
];

assert_methods($expectedClient, CloudRMIClient::class);
assert_methods($expectedServer, CloudRMIServer::class);

fwrite(STDOUT, "All class method availability tests passed\n");
