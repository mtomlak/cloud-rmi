<?php
$autoload = __DIR__ . '/../cloudrmi/vendor/autoload.php';

if (!file_exists($autoload)) {
	fwrite(STDERR, "Run composer install in php/cloudrmi to generate the autoloader.\n");
	exit(1);
}

require_once $autoload;

// Prefer the native extension but allow the polyfill.
if (!extension_loaded('profiweb_cloudrmi') && !extension_loaded('cloudrmi')) {
	$extension = getenv('PROFIWEB_CLOUDRMI_EXT');
	if (!empty($extension) && file_exists($extension)) {
		// Attempt to load the extension dynamically for the current process.
		@dl($extension);
	}
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
