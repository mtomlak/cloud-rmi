<?php
namespace ProfiWeb\Cloud\Infrastructure\System\ProcessControl;

use ProfiWeb\Cloud\Infrastructure\System\ProcessControl\Process;
use ProfiWeb\Cloud\Infrastructure\System\IO\StreamHandle;
use ProfiWeb\Cloud\Infrastructure\System\IO\InputStream;
use ProfiWeb\Cloud\Infrastructure\System\IO\OutputStream;
use ProfiWeb\Cloud\Infrastructure\System\IO\FileInfo;
use ProfiWeb\Cloud\Infrastructure\System\IO\IOException;

/**
 * Helps to launch processes and commands in environment with POSIX capabilities.
 * Returns object of type Process.
 *  
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class PosixProcessBuilder extends ProcessBuilder
{   
    protected $uid;
    protected $gid;
    protected $minUid;
    protected $rootDir;
    protected $fifoDir;
    protected $userPipes;
    protected $proc;

    /**
     * Constructs new ProcessBuilder from command and its args as string
     * 
     */
    public function __construct($command = null)
    {
        parent::__construct($command);

        $this->unsetUid()->unsetGid()->unsetRoot();
    }

    /**
     * Starts a new process using the attributes of this process builder.
     * 
     */
    public function start()
    {
        // To change UID or root directory we need to fork a new child process
        if ($this->haveToSetUid() || $this->haveToSetGid() || $this->haveToChroot()) {
            $this->proc = new ProcessFork();

            $this->createUserPipes();    
            $this->preloadClasses();    // if we chroot or setuid autoloader might stop working
            $this->proc->fork();    

            if ($this->proc->isParent()) { 
                // First we try to contact the child process and check whether
                // it's successfully set-up
                if (!empty($err = $this->userPipes->childError->openRead()->read()))
                    throw new \Exception($err);

                // Connect one end of the pipes
                $outR = $this->userPipes->procOutput->openRead();
                $errR = $this->userPipes->procError->openRead();
                $inW = $this->userPipes->procInput->openWrite();

                // return Process object with corresponding
                return new Process(
                    new SetuidProcessHandle($this->proc),
                    new OutputStream($inW->getFileHandle()),
                    new InputStream($outR->getFileHandle()),
                    new InputStream($errR->getFileHandle())  
                    );

            } else {    
                // Open own output pipe to the parent
                $errOut = $this->userPipes->childError->openWrite();

                // We should send exceptions back to parent process
                try {
                    // Change root
                    if ($this->haveToChroot()) 
                        if (!chroot($this->rootDir)) 
                            throw new \Exception("Cannot launch new process: chroot() failed");   
                        else
                            $this->unsetRoot()->updateUserPipesRoot();  

                    // Change GID
                    if ($this->haveToSetGid())
                        if (!posix_setgid($this->gid))
                            throw new \Exception("Cannot launch new process: setgid() failed");

                    // Change UID
                    if ($this->haveToSetUid()) {
                        $this->checkUid();
                        $this->updateUserPipesUid();

                        if (!posix_setuid($this->uid))
                            throw new \Exception("Cannot launch new process: setuid() failed");
                    }
                } catch (\Exception $e) {
                    $errOut->write($e->getMessage())->close();

                    return $this->proc->join();    // exits the process
                    
                } finally {
                    $errOut->close(); // close if no error yet
                }

                // Connect other end of the pipes
                // Note that the order has to be same as in parent, because opening
                // a pipe will cause blocking till the other end gets connected
                //$errOut = $this->userPipes->childError->openWrite();
                $outW = $this->userPipes->procOutput->openWrite();
                $errW = $this->userPipes->procError->openWrite();
                $inR = $this->userPipes->procInput->openRead();

                $this->redirectInput($inR);
                $this->redirectOutput($outW);
                $this->redirectError($errW);

                $process = parent::start();
                $this->proc->join();  // exit this child process
            }
        } else
            return parent::start();
    }       
        
    /**
     * Gets the value of uid that new process will be launched with
     *
     * @return mixed
     */
    public function getUid()
    {
        if (is_null($this->uid))
            return $this->getCurrentUid();

        return $this->uid;
    }

    /**
     * Gets the value of gid.
     *
     * @return mixed
     */
    public function getGid()
    {
         if (is_null($this->gid))
            return $this->getCurrentGid();

        return $this->gid;
    }

    /**
     * Gets the value of gid.
     *
     * @return mixed
     */
    public function getRoot()
    {
         return (is_null($this->rootDir)) ? new FileInfo('/') : $this->rootDir;
    }

    /**
     * Set the real user ID of a new process. Accepts UID, POSIX username can
     * also be given.
     * 
     * @param int $uid Posix UID
     */
    public function setUid($uid)
    {
        // validation will be done after possible chroot
        $this->uid = $uid;

        return $this;
    }


    /**
     * By calling this method UID of the process will not be changed
     * 
     * @param int $uid Posix UID
     */
    public function unsetUid()
    {
        $this->uid = null;

        return $this;
    }

    /**
     * Sets the minimum allowed value of UID. Default is 1000.
     *
     * @param mixed $minUid Minimum UID that can be set
     *
     * @return self
     */
    public function setMinUid($minUid = 1000)
    {
        $this->minUid = (int) $minUid;

        return $this;
    }

    /**
     * Sets the value of gid.
     *
     * @param mixed $gid the gid
     *
     * @return self
     */
    public function setGid($gid)
    {
        $this->gid = (int) $gid;

        return $this;
    }

    /**
     * By calling this method GID of the process will not be changed.
     *
     * @return self
     */
    public function unsetGid()
    {
        $this->gid = null;

        return $this;
    }

    /**
     * Changes root directory of the new process - POSIX rootDir()
     * 
     */
    public function setRoot($rootDir)
    {
        $this->rootDir = new FileInfo($rootDir);

        if (!$this->rootDir->exists())
            throw new \Exception("New root directory does not exists: $rootDir");

        return $this;
    }

    /**
     * By calling this method GID of the process will not be changed.
     *
     * @return self
     */
    public function unsetRoot()
    {
        $this->rootDir = null;

        return $this;
    }


    /**
     * Sets the value of gid.
     *
     * @param mixed $gid the gid
     *
     * @return self
     */
    public function setFifoDir($fifoDir)
    {
        if (!(new FileInfo($fifoDir))->isDir())
            throw new \Exception("FIFO directory not found: $fifoDir");

        $this->fifoDir = (string) $fifoDir;

        return $this;
    }


    /**
     * Removes temporary pipes that had been created by this class.
     * 
     */
    public function destroyUserPipes()
    {
        if ($this->proc->isParent())
            foreach($this->userPipes as $pipe)
                $pipe->delete();
    }

    /**
     * Gets current real user ID of new process same as parent's
     *
     */
    protected function getCurrentUid()
    {
        return posix_getuid();
    }

    /**
     * Sets group ID of new process same as parent's
     *
     */
    protected function getCurrentGid()
    {
        return posix_getgid();
    }

    /**
     * Checks whether user with set UID or username exists and it's allowed.
     * Converts username to UID if username is given.
     * 
     */
    protected function checkUid()
    {
        if (!is_null($this->uid))
            if (($this->uid = $this->getPosixUser($this->uid)->uid) < $this->minUid)
                throw new \Exception("Cannot set UID to less than " . $this->minUid
                    . ": Change with setMinUid() if you know what you are doing");

        return $this;
    }

    protected function getFifoDir()
    {
        if ($this->fifoDir)
            $path = $this->fifoDir;
        else
            $path = sys_get_temp_dir();  // fallback

        return $this->getRoot()->getChild($path);
    }

    /**
     * Creates FIFO pipes to communicate with child processes 
     * 
     */
    protected function createUserPipes()
    {
        $this->userPipes = (object) array(
            'procInput'     => $this->randomPipe(), 
            'procOutput'    => $this->randomPipe(),
            'procError'     => $this->randomPipe(),
            'childInput'    => $this->randomPipe(),
            'childError'   => $this->randomPipe()
            );
        // make sure pipes get deleted when we no longer need them
        register_shutdown_function(array($this, 'destroyUserPipes'));
    }

    /**
     * Updates path to the pipes after root directory has been changed
     * 
     */
    protected function updateUserPipesRoot()
    {
        foreach($this->userPipes as $name => $value)
            $this->userPipes->$name = $this->getFifoDir()->getChild($value->getBasename());
    }

    /**
     * Changes path to the pipes after root directory has been changed
     * 
     */
    protected function updateUserPipesUid()
    {
        foreach($this->userPipes as $pipe)
            $pipe->setPosixOwner($this->getUid());
    }

    protected function randomPipe()
    {
        $filename = uniqid($this->uid, true) . '.usr.pipe';
        $pipe = $this->getFifoDir()->getChild($filename); 

        $pipe->createNamedPipe();
        $pipe->setPosixOwner($this->getUid(), $this->getGid());

        return $pipe;
    }   

    /**
     * Forces autoloader to load classes needed by child process, after chroot()
     * or setuid() the autloloader might stop working.
     * 
     */ 
    protected function preloadClasses()
    {
        // TODO: find more reliable way
        try {
            get_class_methods(Process::class);
            get_class_methods(ProcessHandle::class);
            get_class_methods(InputStream::class);
            get_class_methods(OutputStream::class);
            get_class_methods(FileStream::class);
        } catch(\Throwable $e) {
            // noop
        }
    }

    /**
     * Returns stdObject representing POSIX user identified by given name or UID.
     * The resulting object has following properties: name, uid, gid, gecos,
     * dir and shell.
     * 
     */
    protected function getPosixUser($uidOrName)
    {
        if (is_numeric($uidOrName)) {
            if (($info = posix_getpwuid((int) $uidOrName)) === false)
                throw new \Exception("User of name not found: $uidOrName");
        } else {
            if (($info = posix_getpwnam((string) $uidOrName)) === false)
                throw new \Exception("User of ID not found: $uidOrName");
        }
        unset($info['passwd']);

        return (object) $info;
    }


    /**
     * Checks whether we have to change UID of a new process
     * 
     */
    protected function haveToSetUid()
    {
        return (!is_null($this->uid));
    }

    /**
     * Checks whether we have to change UID of a new process
     * 
     */
    protected function haveToSetGid()
    {
        return (!is_null($this->gid));
    }

    /**
     * Checks whether we have to chroot() a new process
     * 
     */
    protected function haveToChroot()
    {
        return (!is_null($this->rootDir));
    }
}
