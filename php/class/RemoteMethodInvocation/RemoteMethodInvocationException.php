<?php
namespace ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation;

/**
 * An exception thrown by classes of PosixAccount package
 *  
 * @copyright Copyright (C) 2015 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class RemoteMethodInvocationException extends \Exception
{
	protected $cause;

	/**
	 * Constructs a new exception
	 * 
	 */
	public function __construct($message, $cause = null)
	{
		Parent::__construct($message);
		$this->cause = $cause;	
	}
	
	public function getCause()
	{
		return $this->cause;
	}
	
}
