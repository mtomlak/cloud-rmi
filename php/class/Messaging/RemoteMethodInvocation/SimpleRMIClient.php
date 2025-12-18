<?php
namespace ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation;

require_once __DIR__ . '/CloudRMIClient.php';

if (!class_exists(__NAMESPACE__ . '\\SimpleRMIClient', false)) {
	class_alias(CloudRMIClient::class, __NAMESPACE__ . '\\SimpleRMIClient');
}

