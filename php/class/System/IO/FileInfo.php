<?php
namespace ProfiWeb\Cloud\Infrastructure\System\IO;

/**
 * An abstract representation of file and directory pathnames.
 * Under development. Methods should be added when they will be needed.
 *   
 * 
 * Inspiration for further methods:
 *  https://msdn.microsoft.com/en-us/library/system.io.fileinfo(v=vs.110).aspx 
 *  https://msdn.microsoft.com/en-us/library/system.io.filestream(v=vs.110).aspx
 * more inspiration:
 *  http://docs.oracle.com/javase/7/docs/api/java/io/File.html  
 *    
 *  
 * @copyright Copyright (C) 2015 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class FileInfo
{
	protected $path;
	protected $separator;
	protected $posixOwnerUid;
	protected $posixOwnerGid;
	protected $posixAccessMode;

	/**
	 * Constructs new object of this class
	 * 
	 */
	public function __construct($path)
	{
		$this->setPath($path);
		$this->posixOwnerUid = $this->posixOwnerGid	= $this->posixAccessMode = null;
	}
	
	/**
	 * Opens the file in given mode like FileMode::MODE_OPEN_READ
	 *
	 * @param $mode 
	 * @return FileStream 
	 */	 	 	
	public function open(FileMode $mode)
	{
		$this->assertExists();
		
		if (FileMode::MODE_OPEN_READ)
			$this->assertIsReadable();
		else
			$this->assertIsWritable();

		$systemHandle = fopen($this->path, $mode->getNative());
		$this->assertStateNotFalse($systemHandle, "Error opening file: $this");
		
		return new FileStream(new FileHandle($systemHandle), $mode);
	}
	
	/**
	 * Creates read-only filestream
	 *
	 * @return FileStream          	
	 */	 	 	
	public function openRead()
	{
		return $this->open(new FileMode(FileMode::MODE_OPEN_READ));
	}
	
	/**
	 * Creates write-only filestream
	 *
	 * @return FileStream
	 */	 	 	
	public function openWrite()
	{
		return $this->open(new FileMode(FileMode::MODE_OPEN_WRITE));
	}
	
	/**
	 * Single-shot operation to read file content into the string,
	 * opens the file file for read, loads all contents, and closes the file.
	 * 
	 * @return string
	 */
	public function getContents()
	{
		return $this->openRead()->read();
	}	 	

	/**
	 * Single-shot operation to write file content from the string.
	 * 
	 * @return string
	 */
	public function setContents($string)
	{
		$this->openWrite()->write($string)->close();

		return $this;
	}	 

	/**
	 * Single-shot operation to read file content into the array of strings.
	 * opens the file file for read, loads all contents, and closes the file.
	 * 
	 * @return string
	 */
	public function getAllLines()
	{
		return explode("\n", $this->getContents());
	}	 	 	 	
	
	/**       
	 * Permanently deletes a file 
	 * TODO: check how it works recursivelly
	 *	 
	 * @return void	 
	 */	 	 	
	public function delete()
	{
		if ($this->exists()) {
			$this->assertIsWritable();

			unlink($this->path);
		}
	}
	
	/**
	 * Checks whether file exists
	 * 	 
	 * @return bool	 
	 */	 	 	
	public function exists()
	{
		return file_exists($this->getPath());
	}
	
	/**
	 * Creates a file
	 * 	 	 
	 */	 	 	
	public function createFile($allowOverwrite = false)
	{
		if (!$allowOverwrite)
			$this->assertNotExists();

		if (!touch($this->path))
			throw new IOException("Error creating new file: $this");

		$this->applyPosixOwnership();

		return $this;			
	}

	/**
	 * Creates a random file. 
	 * 	 	 
	 */	 	 	
	public function createRandomFile($prefix = null)
	{
		$dir = ($this->isDir()) ? $this : $this->getParent();

		return new static(tempnam($dir, $prefix));
	}
	
	/**
	 * Creates a directory
	 * Might ask for security info in future	 
	 * 	 	 
	 */	 	 	
	public function createDir($recursive = false)
	{
		// this recursion also takes care about ownership of created files
		if (!$this->getParent()->exists() && $recursive)
			if (!is_null($this->getParentPath())) {
				$parent = $this->getParent();

				if (!is_null($this->posixOwnerUid))
					$parent->setPosixOwner($this->posixOwnerUid, $this->posixOwnerGid);

				$parent->createDir(true);
			}
			else
				throw new IOException('Cannot create path: ' . $this->path);	
		
		$this->assertNotExists();
		$mode = (is_null($this->posixAccessMode)) ? 0775 : $this->posixAccessMode;

		// create the directory or throw an exception
		if (!@mkdir($this->path, $mode))
			throw new IOException('Error creating directory: ' . $this->path);

		$this->applyPosixOwnership();
			
		return $this;
		
	}                                                         
	
	/**
	 * Shortcut to create a directory recursively
	 * 	 	 
	 */	 	 	
	public function createDirectories()
	{
		return $this->createDir(true);
	}


	/**
	 * Creates POSIX named pipe.
	 * 
	 * 
	 */
	public function createNamedPipe()
	{
		$this->assertNotExists();
		$mode = (is_null($this->posixAccessMode)) ? 0600 : $this->posixAccessMode;

		if (!posix_mkfifo($this->path, $mode))
			throw new IOException("Error creating named pipe: $this");

		$this->applyPosixOwnership();

		return $this;
	}

	/**
	 * Creates a symbolic link. Works as UNIX 'ln -s' command.
	 * TODO: Test this
	 * 
	 */
	public function createSymlink($destination)
	{  
		$dest = new self($destination);

		$this->assertExists();

		if ($dest->isDir())
			$dest = $dest->getChild($this->getBasename());	// php needs full destination path, not just dir

		$this->assertArgumentNotExists($dest);

		if (!symlink($this->path, $linkPath))
			throw new IOException("Error creating symbolic link: $this -> $dest");

		return $this;
	}

	/**
	 * Creates a hardlink link. Works as UNIX 'ln' command.
	 * TODO: Test this
	 * 
	 */
	public function createLink($destination)
	{  
		$dest = new self($destination);

		$this->assertExists();

		if ($dest->isDir())
			$dest = $dest->getChild($this->getBasename());

		$this->assertArgumentNotExists($dest);

		if (!link($this->path, $linkPath))
			throw new IOException("Error creating symbolic link: $this -> $dest");

		return $this;
	}

	/**
	 * Copies the file or directory to a another path. Behaviour should be 
	 * as close as possible to standard UNIX command 'cp' with -r (recursive) 
	 * option enabled.
	 * 
	 * Warning: If the destination file already exists, it will be overwritten.
	 * 	 	 
	 */	 	 	
	public function copyTo($destPath)
	{
		$dest = new self($destPath);

		$this->assertExists();

		if ($this->isDir()) { 
			if ($dest->isFile())
				throw new IOException("Cannot overwrite non-directory '$dest' with "
				. " directory '$this'");

			if ($dest->exists())
				$dest = $dest->getChild($this->getBasename());

			else if (!$dest->getParent()->exists())
				throw new IOException("Copy destination not found: $dest");

			// create new destination directory
			$dest->createDir(); 

			foreach($this->listContents() as $child) {
				$child->copyTo($dest); 
			}

		} else {
			if ($dest->isDir())
				$dest = $dest->getChild($this->getBasename());

			else if (!$dest->getParent()->exists())
				throw new IOException("Copy destination not found: $dest");

			if (!copy($this->getPath(), $dest->getPath()))
				throw new IOException("Failed to copy file " . $this->getPath() 
					. " to destination " . $dest->getPath());	
		}
		$this->applyPosixOwnership();

		return $dest;
	}

	/**
	 * Moves file to another destination
	 * 	 	 
	 */	 	 	
	public function moveTo($destPath)
	{
		$dest = new self($destPath);

		$this->assertExists();

		if ($dest->exists())
			throw new IOException("Destination already exists: $dest");

		if (!rename($this->getPath(), $dest->getPath()))
			throw new IOException("Failed to move file " . $this->getPath() 
				. " to destination " . $dest->getPath());	

		$this->applyPosixOwnership();

		return $dest;
	}
	
	/**
	 * Returns parent directory as FileInfo object
	 * 
	 */
	public function getParent()
	{  
		return new self($this->getParentPath());
	}
	
	/**
	 * Returns parent directory path as a string
	 * 
	 */
	public function getParentPath()
	{
		$parent = dirname($this->path);
		
		if ($parent == '//' || $parent == '\\' || $parent == null)
			return null;
			
		return $parent;
	}	 	

	/**
	 * Returns FileInfo of child entry
	 * 
	 */
	public function getChild($filename)
	{
		return new self($this->path . '/' . static::sanitizePath($filename));
	} 	
	
	/**
	 * Returns path of the current for or a directory in a string format 
	 * native to the operating system
	 * 
	 */
	public function getPath()
	{
		return str_replace('/', DIRECTORY_SEPARATOR, $this->path);
	}

	/**
	 * Returns file or directory name without any path component
	 * 
	 */
	public function getBasename()
	{
		return basename($this->getPath());
	}

	/**
	 * Returns file or directory name without any path component
	 * 
	 */
	public function getExtension()
	{
		return pathinfo($this->getPath(), PATHINFO_EXTENSION);
	}

	/**
	 * Returns file or directory name without any path component
	 * 
	 */
	public function getFilename()
	{
		return pathinfo($this->getPath(), PATHINFO_FILENAME);
	}

	/**
	 * Returns file modification time (in unix timestamp seconds)
	 * 
	 */
	public function getModificationTime()
	{
		return filemtime($this->getPath());
	}

	/**
	 * Updates file modification time (in unix timestamp seconds)
	 * 
	 */
	public function setModificationTime($timestamp = null)
	{
		if (!touch($this->path, $timestamp))
			throw new IOException("Error updating modification time of file: $this");

		return $this;
	}

	/**
	 * Tests whether the file denoted by this abstract pathname is a directory
	 *
	 */	 	 
	public function isDir()
	{
		return is_dir($this->path);
	}
	
	/**
	 * Tests whether the file denoted by this abstract pathname is a directory
	 *
	 */	 	 
	public function isFile()
	{
		return is_file($this->path);
	}
	
	/**
	 * Tests whether the file denoted by this abstract pathname is a directory
	 *
	 */	 	 
	public function isLink()
	{
		return is_link($this->path);
	}
	
	/**
	 * Sets ACL of the file.
	 *
	 */	 	 
	public function setAccessControl(FileSecurity $fileSecurity)
	{
	}
	
	/**
	 * Gets ACL of the fil.e
	 *
	 */	 	 
	public function getAccessControl()
	{
	}
	
	/**
	 * Sets a POSIX owner of the file
	 *
	 */	 	 
	public function setPosixOwner($uid, $gid = null, $recursive = false) // todo: recursion
	{
		if ($this->exists()) {
			chown($this->path, $uid);

			if ($this->isDir() && $recursive)
				foreach($this->listContents() as $child)
					$child->setPosixOwner($uid, $gid, true);

		} else 		// doesn't exist yet
			$this->posixOwnerUid = $uid;
		
		if ($gid !== null)
			$this->setPosixGroup($gid);	

		return $this;
	}
	
	/**
	 * Sets a POSIX owner of the file
	 *
	 */	 	 
	public function setPosixGroup($gid)
	{
		if ($this->exists())
			chgrp($this->path, (int) $gid);
		else
			$this->posixOwnerGid = $gid;	

		return $this;
	}
	
	/**
	 * Sets a POSIX mode of the file. The mode should be integer or octal.
	 *
	 */	 	 
	public function setPosixAccessMode($mode)
	{
		if ($this->exists())
			chmod($this->path, $mode);
		else
			$this->posixAccessMode = $mode;	

		return $this;
	}

	
	/**
	 * Gets a POSIX owner of the file
	 *
	 */	 	 
	public function getPosixOwnerUid()
	{
		if ($this->exists())
			return fileowner($this->path);
		else
			return $this->posixOwnerUid;
	}
	
	/**
	 * Gets a POSIX owner of the file
	 *
	 */	 	 
	public function getPosixOwnerGid()
	{
		if ($this->exists())
			return filegroup($this->path);
		else
			return $this->posixOwnerGid;
	}
	
	/**
	 * Gets a POSIX access mode of the file
	 *
	 */	 	 
	public function getPosixAccessMode()
	{
		if ($this->exists())
			return fileperms($this->path);
		else
			return $this->posixAccessMode;
	}
	
	public function listContents($listDirs = true, $listFiles = true)
	{
		$result = array();
		
		$this->assertExists();
		$this->assertIsReadable();

		if (!$this->isDir())
			return null;	// todo: raise an exception
					
		$list = scandir($this->path);
		
		foreach($list as $filename)
			if ($filename != '.' && $filename != '..') {
				$file = new self($this->path . '/' . $filename);

				if ($file->isDir() && $listDirs)
					$result[] = $file;

				if ($file->isFile() && $listFiles)
					$result[] = $file;
			}
			
		return $result;		
	}

	public function listFiles()
	{
		return $this->listContents(false, true);		
	}

	public function listDirs()
	{
		return $this->listContents(true, false);		
	}

	public function toString()
	{
		return $this->getPath();
	}

	public function __toString()
	{
		return $this->getPath();
	}
	
	protected function setPath($path)
	{ 
		$this->path = $this::sanitizePath($path);
	}
	
	protected function applyPosixOwnership()
	{
		if (!is_null($this->posixOwnerUid))
			$this->setPosixOwner($this->posixOwnerUid);

		if (!is_null($this->posixOwnerGid))
			$this->setPosixGroup($this->posixOwnerGid);
	}
	
	protected function assertStateNotFalse($check, $message)
	{
		if ($check === false)
			throw new IOException($message);
	}

	protected function assertExists()
	{
		if (!$this->exists())
			throw new IOException("Path does not exist: $this");
	}

	protected function assertNotExists()
	{
		if ($this->exists())
			throw new IOException("Path already exist: $this");
	}

	protected function assertIsReadable()
	{
		if (@is_readable($this->getPath()) !== true)
			throw new PermissionDeniedException("Read permission denied: $this", $this->getPath());
	}

	protected function assertIsWritable()
	{
		if (@is_writable($this->getPath()) !== true)
			throw new PermissionDeniedException("Write permission denied: $this", $this->getPath());
	}

	protected function assertArgumentExists(FileInfo $file)
	{
		if (!$file->exists())
			throw new IOException("Specified path does not exist: $this");
	}

	protected function assertArgumentNotExists(FileInfo $file)
	{
		if ($file->exists())
			throw new IOException("Specified path already exist: $this");
	}

	static protected function sanitizePath($path)
	{
		// The forward slash "/" is already compatible also with Windows PHP
		// so using DIRECTORY_SEPARATOR is completly unneccessary
		// Edit: but using native separator is cooler, added to output function
		$separator = '/';
		$convert = '\\';
		$simplifyCases = array('///', '//', '/./');
				
		$path = str_replace($convert, $separator, (string) $path);
		$path = str_replace($simplifyCases, $separator, $path);
		
		return $path;	
	}
}
