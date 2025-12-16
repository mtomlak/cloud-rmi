<?php
namespace ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation;

/**
 * Connection objects return operating system's handle to the connection
 *  
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license BSD License
 */
interface ConnectionInterface
{
	public function getSocket();
}
