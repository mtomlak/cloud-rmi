<?php
namespace ProfiWeb\Cloud\Infrastructure\System\IO;

/**
 * Container for OS-specific file security information. Actual implementation
 * provides interface for POSIX-like systems. 
 * 
 * TODO: Make it interface and create more drivers  
 *  
 * @copyright Copyright (C) 2015 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class FileSecurity
{
	protected $posixOwnerUid;
	protected $posixOwnerGid;
	protected $posixMode;
	
	/**
	 * Constructs new object of this class
	 * 
	 * TODO: validation	 	 
	 * 
	 */
	public function __construct($uid, $gid, $mode)
	{
		$this->setPosixOwnerUid = $uid;
		$this->setPosixOwnerGid = $gid;
		$this->setPosixMode = $mode;	
	}
	 	 	 	
	/**
	 * Gets an owners POSIX UID
	 *
	 * @return int 
	 */	 	 	
	public function getPosixUid($mode)
	{
		return $this->posixOwnerUid;
	}
	
	/**
	 * Gets an owners POSIX UID
	 *
	 * @return int 
	 */	 	 	
	public function getPosixGid($mode)
	{
		return $this->posixOwnerGid;
	}
	
	/**
	 * Gets an owners POSIX UID
	 *
	 * @return int 
	 */	 	 	
	public function getPosixMode($mode)
	{
		return $this->posixMode;
	}
	
	/**
	 * Gets an owners POSIX UID
	 *
	 * @return int 
	 */	 	 	
	protected function setPosixUid($uid)
	{
		$this->posixOwnerUid = (int) $uid;
	}
	
	/**
	 * Gets an owners POSIX UID
	 *
	 * @return int 
	 */	 	 	
	protected function setPosixGid($gid)
	{
		$this->posixOwnerGid = (int) $gid;
	}	
	
	/**
	 * Gets an owners POSIX UID
	 *
	 * @return int 
	 */	 	 	
	public function setPosixMode($mode)
	{
		$this->posixMode = $mode;
	}

}
