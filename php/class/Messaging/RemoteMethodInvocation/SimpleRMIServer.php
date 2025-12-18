<?php
namespace ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation;

require_once __DIR__ . '/CloudRMIServer.php';

if (!class_exists(__NAMESPACE__ . '\\SimpleRMIServer', false)) {
	class_alias(CloudRMIServer::class, __NAMESPACE__ . '\\SimpleRMIServer');
}

