<?php
namespace ProfiWeb\Cloud\Infrastructure\System\IO;

/**
 * Value object to represent data size
 *  
 * @copyright Copyright (C) 2021 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class DataSize
{	
	const BYTES_PER_KB = 1024;
	const BYTES_PER_MB = 1024 * 1024;
	const BYTES_PER_GB = 1024 * 1024 * 1024;
	const BYTES_PER_TB = 1024 * 1024 * 1024 * 1024;

	protected $bytes;

	/**
	 * Creates the value object. 
	 * 
	 * @param string $principalName
	 * @return void	 
	 */	 	 	 		
	public function __construct($bytes)
	{
		$this->bytes = (int) $bytes;
	} 
	
	public function toBytes()
	{
		return $this->bytes;
	}

	public function toKiloBytes()
	{
		if (!$this->bytes)
			return 0;
	
		return $this->bytes / static::BYTES_PER_KB;
	}

	public function toMegaBytes()
	{
		
		if (!$this->bytes)
			return 0;
	
		return $this->bytes / static::BYTES_PER_MB;
	}

	public function toGigaBytes()
	{
		
		if (!$this->bytes)
			return 0;
	
		return $this->bytes / static::BYTES_PER_GB;
	}

	public function toTeraBytes()
	{
		
		if (!$this->bytes)
			return 0;
	
		return $this->bytes / static::BYTES_PER_TB;
	}

	public function add(DataSize $subject)
	{
		return new static($this->bytes + $subject->toBytes());
	}

	public function substract(DataSize $subject)
	{
		return new static($this->bytes - $subject->toBytes());
	}

	public function isNegative()
	{
		return $this->bytes < 0;
	}
	
	public function toString()
	{
		return (string) $this->bytes;
	}

	public function __toString()
	{
		return $this->toString();
	}

	public static function ofBytes($b)
	{
		return new DataSize($b);
	}

	public static function ofKiloBytes($kb)
	{
		return new DataSize($kb * static::BYTES_PER_KB);
	}

	public static function ofMegaBytes($mb)
	{
		return new DataSize($mb * static::BYTES_PER_MB);
	}

	public static function ofGigaBytes($gb)
	{
		return new DataSize($gb * static::BYTES_PER_GB);
	}

	public static function ofTeraBytes($tb)
	{
		return new DataSize($tb * static::BYTES_PER_TB);
	}
}
