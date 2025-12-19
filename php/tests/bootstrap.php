<?php
$autoload = __DIR__ . '/../cloudrmi/vendor/autoload.php';

if (!file_exists($autoload)) {
	fwrite(STDERR, "Run composer install in php/cloudrmi to generate the autoloader.\n");
	exit(1);
}

require_once $autoload;

if (!extension_loaded('cloudrmi')) {
	fwrite(STDERR, "cloudrmi extension is required; build it via make -C php build-ext.\n");
	exit(1);
}

function cloudrmi_test_command(string $script, array $args = []): array
{
	$cmd = [PHP_BINARY];
	$extension = getenv('PROFIWEB_CLOUDRMI_EXT');

	if (!empty($extension)) {
		$cmd[] = '-d';
		$cmd[] = 'extension=' . $extension;
	}

	$cmd[] = $script;

	return array_merge($cmd, $args);
}
