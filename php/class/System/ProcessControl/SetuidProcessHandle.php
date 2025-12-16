<?php
namespace ProfiWeb\Cloud\Infrastructure\System\ProcessControl;

/**
 * Setuid process is a process whose parent had to fork in order to setuid() 
 * before actually launching the process this handle referes to. Hence we keep
 * also reference to parent's fork object to make it wait for a child if needed.
 *  
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class SetuidProcessHandle extends ProcessHandle
{	
	protected $parentProcFork;

	/**
	 * Constructs new object of this class
	 * 
	 */
	public function __construct(ProcessFork $parentFork)
	{
		//$this->handle = $nativeHandle;	
		$this->parentProcFork = $parentFork;
	}
	
	/**
	 * Releases all resources this handle represents
	 * 
	 */
	public function close()
	{
		return proc_terminate($this->handle);
	}	 	 	

	public function waitFor()
	{
		return $this->parentProcFork->join();
	}	
}
