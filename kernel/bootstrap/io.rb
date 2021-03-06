class IO

  class Buffer
    def self.allocate
      Ruby.primitive :iobuffer_allocate
      raise PrimitiveFailure, "IO::Buffer.allocate primitive failed"
    end
  end

  def self.allocate
    Ruby.primitive :io_allocate
    raise PrimitiveFailure, "IO.allocate primitive failed"
  end

  def self.open_with_mode(path, mode, perm)
    Ruby.primitive :io_open
    raise PrimitiveFailure, "IO.open_with_mode primitive failed"
  end

  def self.connect_pipe(lhs, rhs)
    Ruby.primitive :io_connect_pipe
    raise PrimitiveFailure, "IO.connect_pipe primitive failed"
  end

  def ensure_open
    Ruby.primitive :io_ensure_open
    raise PrimitiveFailure, "IO#ensure_open primitive failed"
  end

  def write(str)
    Ruby.primitive :io_write
    raise PrimitiveFailure, "IO#write failed. Might not have passed a string."
  end

  def blocking_read(size)
    Ruby.primitive :io_blocking_read
    raise PrimitiveFailure, "IO#blocking_read primitive failed"
  end

  def prim_reopen(other)
    Ruby.primitive :io_reopen
    raise ArgumentError, "IO#prim_reopen only accepts an IO object"
  end

  def prim_seek(amount, whence)
    Ruby.primitive :io_seek
    raise PrimitiveFailure, "IO#prim_seek primitive failed"
  end

  def query(which)
    Ruby.primitive :io_query
    raise PrimitiveFailure, "IO#query primitive failed"
  end

  def reopen(other)
    prim_reopen other
  end

  def tty?
    query :tty?
  end

  def ttyname
    query :ttyname
  end

  def close
    Ruby.primitive :io_close
    raise PrimitiveFailure, "IO#close primitive failed"
  end

  #
  # Close read and/or write stream of a full-duplex descriptor.
  #
  # @todo   More documentation. Much more. --rue
  #
  def shutdown(how)
    Ruby.primitive :io_shutdown
  end

end
