<?php
require_once __DIR__ . '/../../php/class/RemoteMethodInvocation/RemoteMethodInvocationException.php';
require_once __DIR__ . '/../../php/class/RemoteMethodInvocation/RemoteErrorException.php';
require_once __DIR__ . '/../../php/class/RemoteMethodInvocation/RemoteException.php';
require_once __DIR__ . '/../../php/class/RemoteMethodInvocation/ConnectionInterface.php';
require_once __DIR__ . '/../../php/class/RemoteMethodInvocation/SimpleRMIConnection.php';
require_once __DIR__ . '/../../php/class/RemoteMethodInvocation/SimpleRMIClient.php';

use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\SimpleRMIClient;
use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\RemoteMethodInvocationException;

[$script, $host, $port, $token, $namespace] = $argv + [null, null, null, null, null];

try {
    $client = new SimpleRMIClient($host, (int) $port, false, 5);
    $client->connect();
    $client->authByToken($token);

    $iface = "{$namespace}\\PayloadInterface";

    $first = $client->callRemoteMethod($iface, 'empty', '', ['payload' => true, 'encoding' => 'RAW']);
    $second = $client->callRemoteMethod($iface, 'uppercase', '', ['payload' => true, 'encoding' => 'RAW']);

    echo "first_len:" . strlen((string) $first) . "\n";
    echo "second:" . $second . "\n";
} catch (RemoteMethodInvocationException $e) {
    fwrite(STDERR, $e->getMessage() . "\n");
    exit(1);
}
