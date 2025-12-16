<?php
namespace ProfiWeb\Cloud\Infrastructure\System;

/**
 * Helps to work with system's environment variables.
 *
 * TODO: variable name validation.
 *   
 * @copyright Copyright (C) 2018 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class Environment
{
	public static function get($name)
	{
		if ($var = getenv($name))
			return $var;

		if (isset($_ENV[$name]) && !empty($_ENV[$name]))
			return $_ENV[$name];

		if (isset($_SERVER[$name]) && !empty($_SERVER[$name]))
			return $_SERVER[$name];

		return null;
	}

	public static function set($name, $value)
	{
		global $_ENV, $_SERVER;

		// putenv/getenv, $_ENV, nd phpinfo(INFO_ENVIRONMENT)
		// are three completely distinct environment stores.
		putenv($name . '=' .  $value);
		$_ENV[$name] = $value;
		$_SERVER[$name] = $value;
	}
}
