<?php
namespace ProfiWeb\Cloud\Infrastructure\System\ProcessControl;

/**
 * Abstraction of low level resources / handles of the language.
 *  
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class ProcessHandle
{	
	protected $handle;

	/**
	 * Constructs new object of this class
	 * 
	 */
	public function __construct($nativeHandle)
	{
		$this->handle = $nativeHandle;	
	}
	 	 	 	
	/**
	 * Gets PHP native file-handle
	 *
	 * @return mixed 
	 */	 	 	
	public function getNativeHandle()
	{
		return $this->handle;
	}
	
	/**
	 * Releases all resources this handle represents
	 * 
	 */
	public function close()
	{
		return proc_terminate($this->handle);
	}	 

	/**
	 * Waits for the process to exit
	 * 
	 */
	public function waitFor()
	{
		// not implemented for generic type of process yet
	}		 	
}
