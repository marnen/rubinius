##
# Used to implement Module#autoload.

class Autoload
  attr_reader :name
  attr_reader :scope
  attr_reader :path
  attr_reader :original_path

  def initialize(name, scope, path)
    @name = name
    @scope = scope
    @original_path = path
    @path, = __split_path__(path)
    Autoload.add(self)
  end

  ##
  # When any code that finds a constant sees an instance of Autoload as its match,
  # it calls this method on us
  def call
    require(path)
    scope.const_get(name)
  end

  ##
  # Called by Autoload.remove
  def discard
    scope.__send__(:remove_const, name)
  end

  ##
  # Class methods
  class << self
    ##
    # Initializes as a Hash with an empty array as the default value
    def autoloads
      @autoloads ||= Hash.new {|h,k| h[k] = Array.new }
    end

    ##
    # Called by Autoload#initialize
    def add(al)
      autoloads[al.path] << al
    end

    ##
    # Called by require; see kernel/common/compile.rb
    def remove(path)
      al = autoloads.delete(path)
      return unless al
      al.each {|a| a.discard }
    end
  end
end
