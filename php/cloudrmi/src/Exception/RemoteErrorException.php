<?php
namespace ProfiWeb\CloudRMI\Exception;

/**
 * Exception forwarded to remote system when error has occoured during
 * remote method invocation.
 *  
 * @copyright Copyright (C) 2016 - 2024 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class RemoteErrorException extends \Exception
{
	protected $errorCode;

	/**
	 * Constructs a new exception from array containing parameters of error.
	 * 
	 */
	public function __construct(
		$errorMessage,
		$errorCode = null
		)
	{
		Parent::__construct($errorMessage);
		$this->errorCode = $errorCode;
	}
	
	public function getErrorCode()
	{
		return $this->errorCode;
	}
}
