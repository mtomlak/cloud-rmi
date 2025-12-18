<?php

function cloudrmi_extension_path(): string
{
    $envPath = getenv('CLOUDRMI_EXTENSION');
    if (is_string($envPath) && $envPath !== '' && file_exists($envPath)) {
        return $envPath;
    }

    $default = dirname(__DIR__, 2) . '/ext/profiweb_cloudrmi/modules/profiweb_cloudrmi.so';
    if (file_exists($default)) {
        return $default;
    }

    fwrite(STDERR, "cloudrmi extension not found. Build php/ext/profiweb_cloudrmi or set CLOUDRMI_EXTENSION.\n");
    exit(1);
}

function ensure_cloudrmi_extension_loaded(): string
{
    $path = cloudrmi_extension_path();
    if (!extension_loaded('cloudrmi')) {
        fwrite(STDERR, "cloudrmi extension must be loaded (try php -d extension={$path}).\n");
        exit(1);
    }

    return $path;
}

function cloudrmi_extension_cli(string $path = null): array
{
    $resolved = $path ?? cloudrmi_extension_path();
    return ['-d', "extension={$resolved}"];
}
