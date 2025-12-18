<?php
require_once __DIR__ . '/../support/extension_helpers.php';
require_once __DIR__ . '/../support/compat_classes.php';

use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\CloudRMIClient;
use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\NativeRMIClient;
use ProfiWeb\Cloud\Tests\Compat\CompatTestInterface;
use ProfiWeb\Cloud\Tests\Compat\NamedValue;

ensure_cloudrmi_extension_loaded();

require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/ConnectionInterface.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/NativeRMIClient.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/SimpleRMIConnection.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteException.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteErrorException.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteMethodInvocationException.php';
require_once __DIR__ . '/../../class/System/ProcessControl/ProcessFork.php';

function assert_equal($expected, $actual, string $message): void
{
    if ($expected != $actual) {
        throw new RuntimeException($message . ': ' . var_export($actual, true));
    }
}

function wait_for_server(string $host, int $port, int $retries = 25): void
{
    for ($i = 0; $i < $retries; $i++) {
        $sock = @stream_socket_client("tcp://{$host}:{$port}", $errno, $errstr, 0.2);
        if ($sock !== false) {
            $line = fgets($sock, 2048);
            fclose($sock);
            if (is_string($line) && strpos($line, '220') === 0) {
                return;
            }
        }
        usleep(100000);
    }

    throw new RuntimeException("server {$host}:{$port} did not start");
}

function start_compat_server(string $mode, int $port, string $extPath, string $token): array
{
    $cmd = ['php'];
    if ($mode === 'extension') {
        $cmd = array_merge($cmd, cloudrmi_extension_cli($extPath));
    }

    $cmd[] = __DIR__ . '/../support/compat_server.php';
    $cmd[] = '--mode=' . $mode;
    $cmd[] = '--port=' . $port;
    $cmd[] = '--token=' . $token;

    $descriptors = [
        0 => ['pipe', 'r'],
        1 => ['pipe', 'w'],
        2 => ['pipe', 'w'],
    ];

    $proc = proc_open($cmd, $descriptors, $pipes);
    if (!is_resource($proc)) {
        throw new RuntimeException("failed to start {$mode} compat server");
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

$extPath = cloudrmi_extension_path();
$token = 'native-ref-token';
$iface = CompatTestInterface::class;
$nativePort = 19000 + random_int(0, 1000);
$extensionPort = 20000 + random_int(0, 1000);

[$nativeProc, $nativePipes] = start_compat_server('native', $nativePort, $extPath, $token);
[$extensionProc, $extensionPipes] = start_compat_server('extension', $extensionPort, $extPath, $token);

try {
    wait_for_server('127.0.0.1', $nativePort);
    wait_for_server('127.0.0.1', $extensionPort);

    $nativeClient = new NativeRMIClient('127.0.0.1', $nativePort);
    $nativeClient->connect();
    $nativeClient->authByToken($token);

    $extensionClient = new CloudRMIClient('127.0.0.1', $extensionPort);
    $extensionClient->connect();
    $extensionClient->authByToken($token);

    $nativeBaselineJoin = $nativeClient->callRemoteMethod($iface, 'joinParts', ['alpha.tld', 'selector', 'dkim']);
    $nativeBaselineSpread = $nativeClient->callRemoteMethod($iface, 'multiArgSpread', ['first', 'second', 'third']);
    $nativeBaselineWrapped = $nativeClient->callRemoteMethod(
        $iface,
        'wrapComplex',
        ['wrap', new NamedValue('nv', ['scope' => 'test'])]
    );

    assert_equal($nativeBaselineJoin, $extensionClient->call($iface, 'joinParts', ['alpha.tld', 'selector', 'dkim']), 'extension server joinParts mismatch');
    assert_equal($nativeBaselineSpread, $extensionClient->call($iface, 'multiArgSpread', ['first', 'second', 'third']), 'extension server multiArgSpread mismatch');
    assert_equal($nativeBaselineWrapped, $extensionClient->call($iface, 'wrapComplex', ['wrap', new NamedValue('nv', ['scope' => 'test'])]), 'extension server wrapComplex mismatch');

    $extensionAgainstNative = new CloudRMIClient('127.0.0.1', $nativePort);
    $extensionAgainstNative->connect();
    $extensionAgainstNative->authByToken($token);

    assert_equal($nativeBaselineJoin, $extensionAgainstNative->call($iface, 'joinParts', ['alpha.tld', 'selector', 'dkim']), 'native server joinParts mismatch for extension client');
    assert_equal($nativeBaselineSpread, $extensionAgainstNative->call($iface, 'multiArgSpread', ['first', 'second', 'third']), 'native server multiArgSpread mismatch for extension client');
    assert_equal($nativeBaselineWrapped, $extensionAgainstNative->call($iface, 'wrapComplex', ['wrap', new NamedValue('nv', ['scope' => 'test'])]), 'native server wrapComplex mismatch for extension client');

    $largeLength = 1500;
    $nativeLarge = $nativeClient->callRemoteMethod($iface, 'largeData', $largeLength);
    if (strlen($nativeLarge) !== $largeLength) {
        throw new RuntimeException('native baseline largeData length mismatch');
    }

    $extensionLarge = $extensionAgainstNative->call($iface, 'largeData', $largeLength);
    assert_equal($nativeLarge, $extensionLarge, 'native server largeData mismatch for extension client');

    $followUp = $extensionAgainstNative->call($iface, 'joinParts', ['follow', 'up', 'check']);
    assert_equal('follow|up|check', $followUp, 'native server follow-up call after large payload failed');

    $nativeAgainstExtension = new NativeRMIClient('127.0.0.1', $extensionPort);
    $nativeAgainstExtension->connect();
    $nativeAgainstExtension->authByToken($token);

    assert_equal($nativeBaselineJoin, $nativeAgainstExtension->callRemoteMethod($iface, 'joinParts', ['alpha.tld', 'selector', 'dkim']), 'extension server joinParts mismatch for native client');
    assert_equal($nativeBaselineSpread, $nativeAgainstExtension->callRemoteMethod($iface, 'multiArgSpread', ['first', 'second', 'third']), 'extension server multiArgSpread mismatch for native client');
    assert_equal($nativeBaselineWrapped, $nativeAgainstExtension->callRemoteMethod($iface, 'wrapComplex', ['wrap', new NamedValue('nv', ['scope' => 'test'])]), 'extension server wrapComplex mismatch for native client');

    $nativeAgainstExtension->disconnect();
    $extensionAgainstNative->disconnect();
    $nativeClient->disconnect();
    $extensionClient->disconnect();

    fwrite(STDOUT, "Native compatibility tests passed\n");
} finally {
    stop_server($nativeProc, $nativePipes);
    stop_server($extensionProc, $extensionPipes);
}
