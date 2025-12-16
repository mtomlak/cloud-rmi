<?php
namespace ProfiWeb\Cloud\Infrastructure\System\IO;

/**
 * An exception thrown by classes of IO package
 *  
 * @copyright Copyright (C) 2017 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class PermissionDeniedException extends IOException
{
	protected $path;

	/**
	 * Constructs a new exception
	 * 
	 */
	public function __construct($message, $path = null)
	{
		parent::__construct($message, null);
		$this->path = $path;	
	}
	
	public function getPath()
	{
		return $this->path;
	}
	
}
