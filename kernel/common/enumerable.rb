# depends on: module.rb class.rb

##
#  The Enumerable mixin provides collection classes with  several traversal
#  and searching methods, and with the ability to sort. The class must provide
#  a method #each, which yields successive members of the collection. If
#  Enumerable#max, #min, or #sort is used, the objects in the collection must
#  also implement a meaningful <tt><=></tt> operator, as these methods rely on
#  an ordering between members of the collection.
#--
# Just to save you 10 seconds, the reason we always use #each to extract
# elements instead of something simpler is because Enumerable can not assume
# any other methods than #each. If needed, class-specific versions of any of
# these methods can be written *in those classes* to override these.

module Enumerable

  class Sort

    def initialize(sorter = nil)
      @sorter = sorter
    end

    def sort(xs, &prc)
      # The ary should be inmutable while sorting
      prc = Proc.new { |a,b| a <=> b } unless block_given?

      if @sorter
        @sorter = method(@sorter) unless @sorter.respond_to?(:call)
        @sorter.call(xs, &prc)
      else
        quicksort(xs, &prc)
      end
    end

    alias_method :call, :sort

    class SortedElement
      def initialize(val, sort_id)
        @value, @sort_id = val, sort_id
      end

      attr_reader :value
      attr_reader :sort_id

      def <=>(other)
        @sort_id <=> other.sort_id
      end
    end

    def sort_by(xs)
      # The ary and its elements sould be inmutable while sorting

      elements = xs.map { |x| SortedElement.new(x, yield(x)) }
      sort(elements).map { |e| e.value }
    end

    ##
    # Sort an Enumerable using simple quicksort (not optimized)

    def quicksort(xs, &prc)
      return [] unless xs

      pivot = Undefined
      xs.each { |o| pivot = o; break }
      return xs if pivot.equal? Undefined

      lmr = xs.group_by do |o|
        if o.equal?(pivot)
          0
        else
          yield(o, pivot)
        end
      end

      quicksort(lmr[-1], &prc) + lmr[0] + quicksort(lmr[1], &prc)
    end

  end

  ##
  # :call-seq:
  #   enum.to_a      =>    array
  #   enum.entries   =>    array
  #
  # Returns an array containing the items in +enum+.
  #
  #   (1..7).to_a                       #=> [1, 2, 3, 4, 5, 6, 7]
  #   { 'a'=>1, 'b'=>2, 'c'=>3 }.to_a   #=> [["a", 1], ["b", 2], ["c", 3]]

  def to_a
    collect { |e| e }
  end

  alias_method :entries, :to_a

  ##
  # :call-seq:
  #   enum.grep(pattern)                   => array
  #   enum.grep(pattern) { | obj | block } => array
  #
  # Returns an array of every element in +enum+ for which <tt>Pattern ===
  # element</tt>. If the optional +block+ is supplied, each matching element
  # is passed to it, and the block's result is stored in the output array.
  #
  #   (1..100).grep 38..44   #=> [38, 39, 40, 41, 42, 43, 44]
  #   c = IO.constants
  #   c.grep(/SEEK/)         #=> ["SEEK_END", "SEEK_SET", "SEEK_CUR"]
  #   res = c.grep(/SEEK/) { |v| IO.const_get(v) }
  #   res                    #=> [2, 0, 1]

  def grep(pattern)
    ary = []
    each do |o|
      if pattern === o
        ary << (block_given? ? yield(o) : o)
      end
    end
    ary
  end

  def sorter
    Enumerable::Sort.new
  end

  ##
  # :call-seq:
  #   enum.sort                     => array
  #   enum.sort { | a, b | block }  => array
  #
  # Returns an array containing the items in +enum+ sorted, either according
  # to their own <tt><=></tt> method, or by using the results of the supplied
  # block. The block should return -1, 0, or +1 depending on the comparison
  # between +a+> and +b+.  As of Ruby 1.8, the method Enumerable#sort_by
  # implements a built-in Schwartzian Transform, useful when key computation
  # or comparison is expensive..
  #
  #   %w(rhea kea flea).sort         #=> ["flea", "kea", "rhea"]
  #   (1..10).sort { |a,b| b <=> a}  #=> [10, 9, 8, 7, 6, 5, 4, 3, 2, 1]

  def sort(&prc)
    sorter.sort(self, &prc)
  end

  ##
  # :call-seq:
  #   enum.sort_by { | obj | block }    => array
  #
  # Sorts +enum+ using a set of keys generated by mapping the
  # values in +enum+ through the given block.
  #
  #   %w{ apple pear fig }.sort_by { |word| word.length}
  #   #=> ["fig", "pear", "apple"]
  #
  # The current implementation of sort_by generates an array of tuples
  # containing the original collection element and the mapped value. This makes
  # sort_by fairly expensive when the keysets are simple
  #
  #   require 'benchmark'
  #   include Benchmark
  #   
  #   a = (1..100000).map {rand(100000)}
  #   
  #   bm(10) do |b|
  #     b.report("Sort")    { a.sort }
  #     b.report("Sort by") { a.sort_by { |a| a} }
  #   end
  #
  # produces:
  #
  #   user     system      total        real
  #   Sort        0.180000   0.000000   0.180000 (  0.175469)
  #   Sort by     1.980000   0.040000   2.020000 (  2.013586)
  #
  # However, consider the case where comparing the keys is a non-trivial
  # operation. The following code sorts some files on modification time
  # using the basic sort method.
  #
  #   files = Dir["#"]
  #   sorted = files.sort { |a,b| File.new(a).mtime <=> File.new(b).mtime}
  #   sorted   #=> ["mon", "tues", "wed", "thurs"]
  #
  # This sort is inefficient: it generates two new File objects during every
  # comparison. A slightly better technique is to use the Kernel#test method
  # to generate the modification times directly.
  #
  #   files = Dir["#"]
  #   sorted = files.sort { |a,b|
  #     test(?M, a) <=> test(?M, b)
  #   }
  #   sorted   #=> ["mon", "tues", "wed", "thurs"]
  #
  # This still generates many unnecessary Time objects. A more efficient
  # technique is to cache the sort keys (modification times in this case)
  # before the sort. Perl users often call this approach a Schwartzian
  # Transform, after Randal Schwartz. We construct a temporary array, where
  # each element is an array containing our sort key along with the filename.
  # We sort this array, and then extract the filename from the result.
  #
  #   sorted = Dir["#"].collect { |f|
  #      [test(?M, f), f]
  #   }.sort.collect { |f| f[1] }
  #   sorted   #=> ["mon", "tues", "wed", "thurs"]
  #
  # This is exactly what sort_by does internally.
  #
  #   sorted = Dir["#"].sort_by { |f| test(?M, f)}
  #   sorted   #=> ["mon", "tues", "wed", "thurs"]

  def sort_by(&prc)
    sorter.sort_by(self, &prc)
  end

  ##
  # :call-seq:
  #   enum.count(item)             => int
  #   enum.count { | obj | block } => int
  #
  # Returns the number of items in +enum+ for which equals to +item+. If a
  # block is given, counts the number of elements yielding a true value.
  #
  #   ary = [1, 2, 4, 2]
  #   ary.count(2)          # => 2
  #   ary.count{ |x|x%2==0}  # => 3

  def count(item = Undefined)
    seq = 0
    unless item.equal? Undefined
      each { |o| seq += 1 if item == o }
    else
      each { |o| seq += 1 if yield(o) }
    end
    seq
  end

  ##
  # :call-seq:
  #   enum.detect(ifnone = nil) { | obj | block }  => obj or nil
  #   enum.find(ifnone = nil)   { | obj | block }  => obj or nil
  #
  # Passes each entry in +enum+ to +block+>. Returns the first for which
  # +block+ is not false.  If no object matches, calls +ifnone+ and returns
  # its result when it is specified, or returns nil
  #
  #   (1..10).detect  { |i| i % 5 == 0 and i % 7 == 0 }   #=> nil
  #   (1..100).detect { |i| i % 5 == 0 and i % 7 == 0 }   #=> 35

  def find(ifnone = nil)
    each { |o| return o if yield(o) }
    ifnone.call if ifnone
  end

  alias_method :detect, :find

  ##
  # :call-seq:
  #   enum.find_index(ifnone = nil)   { | obj | block }  => int
  #
  # Passes each entry in +enum+ to +block+. Returns the index for the first
  # for which +block+ is not false. If no object matches, returns
  # nil.
  #
  #   (1..10).find_index  { |i| i % 5 == 0 and i % 7 == 0 }   #=> nil
  #   (1..100).find_index { |i| i % 5 == 0 and i % 7 == 0 }   #=> 35

  def find_index(ifnone = nil)
    idx = -1
    each { |o| idx += 1; return idx if yield(o) }
    ifnone.call if ifnone
  end

  ##
  # :call-seq:
  #   enum.find_all { | obj | block }  => array
  #   enum.select   { | obj | block }  => array
  #
  # Returns an array containing all elements of +enum+ for which +block+ is
  # not false (see also Enumerable#reject).
  #
  #   (1..10).find_all { |i|  i % 3 == 0 }   #=> [3, 6, 9]

  def find_all
    ary = []
    each do |o|
      if yield(o)
        ary << o
      end
    end
    ary
  end

  alias_method :select, :find_all

  ##
  # :call-seq:
  #   enum.reject { | obj | block }  => array
  #
  # Returns an array for all elements of +enum+ for which +block+ is false
  # (see also Enumerable#find_all).
  #
  #    (1..10).reject { |i|  i % 3 == 0 }   #=> [1, 2, 4, 5, 7, 8, 10]

  def reject
    ary = []
    each do |o|
      unless yield(o)
        ary << o
      end
    end
    ary
  end

  ##
  # :call-seq:
  #   enum.collect { | obj | block }  => array
  #   enum.map     { | obj | block }  => array
  #
  # Returns a new array with the results of running +block+ once for every
  # element in +enum+.
  #
  #   (1..4).collect { |i| i*i }   #=> [1, 4, 9, 16]
  #   (1..4).collect { "cat"  }   #=> ["cat", "cat", "cat", "cat"]

  def collect
    ary = []
    if block_given?
      each { |o| ary << yield(o) }
    else
      each { |o| ary << o }
    end
    ary
  end

  alias_method :map, :collect

  ##
  # :call-seq:
  #   enum.inject(initial) { | memo, obj | block }  => obj
  #   enum.inject          { | memo, obj | block }  => obj
  #
  # Combines the elements of +enum+ by applying the block to an accumulator
  # value (+memo+) and each element in turn. At each step, +memo+ is set
  # to the value returned by the block. The first form lets you supply an
  # initial value for +memo+. The second form uses the first element of the
  # collection as a the initial value (and skips that element while
  # iterating).
  #
  # Sum some numbers:
  #
  #   (5..10).inject { |sum, n| sum + n }              #=> 45
  #
  # Multiply some numbers:
  #
  #   (5..10).inject(1) { |product, n| product * n }   #=> 151200
  #
  # Find the longest word:
  #
  #   longest = %w[ cat sheep bear ].inject do |memo,word|
  #      memo.length > word.length ? memo : word
  #   end
  #   
  #   longest                                         #=> "sheep"
  #
  # Find the length of the longest word:
  #
  #   longest = %w[ cat sheep bear ].inject(0) do |memo,word|
  #      memo >= word.length ? memo : word.length
  #   end
  #   
  #   longest                                         #=> 5

  def inject(memo = Undefined)
    each { |o|
      if memo.equal? Undefined
        memo = o
      else
        memo = yield(memo, o)
      end
    }

    memo.equal?(Undefined) ? nil : memo
  end

  ##
  # :call-seq:
  #   enum.partition { | obj | block }  => [ true_array, false_array ]
  #
  # Returns two arrays, the first containing the elements of +enum+ for which
  # the block evaluates to true, the second containing the rest.
  #
  #   (1..6).partition { |i| (i&1).zero?}   #=> [[2, 4, 6], [1, 3, 5]]

  def partition
    left = []
    right = []
    each { |o| yield(o) ? left.push(o) : right.push(o) }
    return [left, right]
  end

  ##
  # :call-seq:
  #   enum.group_by { | obj | block }  => a_hash
  #
  # Returns a hash, which keys are evaluated result from the block, and values
  # are arrays of elements in +enum+ corresponding to the key.
  #
  #    (1..6).group_by { |i| i%3}   #=> {0=>[3, 6], 1=>[1, 4], 2=>[2, 5]}

  def group_by
    h = {}
    i = 0
    each do |o|
      key = yield(o)
      if h.key?(key)
        h[key] << o
      else
        h[key] = [o]
      end
    end
    h
  end

  ##
  # :call-seq:
  #   enum.first      => obj or nil
  #   enum.first(n)   => an_array
  #
  # Returns the first element, or the first +n+ elements, of the enumerable.
  # If the enumerable is empty, the first form returns nil, and the second
  # form returns an empty array.

  def first(n = nil)
    if n && n < 0
      raise ArgumentError, "Invalid number of elements given."
    end
    ary = []
    each do |o|
      return o unless n
      return ary if ary.size == n
      ary << o
    end
    n ? ary : nil
  end

  # :call-seq:
  #   enum.all?                     => true or false
  #   enum.all? { |obj| block }   => true or false
  #
  # Passes each element of the collection to the given block. The method
  # returns true if the block never returns false or nil. If the block is not
  # given, Ruby adds an implicit block of <tt>{ |obj| obj }</tt> (that is all?
  # will return true only if none of the collection members are
  # false or nil.)
  #
  #   %w[ant bear cat].all? { |word| word.length >= 3}   #=> true
  #   %w[ant bear cat].all? { |word| word.length >= 4}   #=> false
  #   [ nil, true, 99 ].all?                             #=> false

  def all?
    if block_given?
      each { |e| return false unless yield(e) }
    else
      each { |e| return false unless e }
    end
    true
  end

  ##
  # :call-seq:
  #    enum.any? [{ |obj| block } ]   => true or false
  #
  # Passes each element of the collection to the given block. The method
  # returns true if the block ever returns a value other than false or nil. If
  # the block is not given, Ruby adds an implicit block of <tt>{ |obj| obj
  # }</tt> (that is any? will return true if at least one of the collection
  # members is not false or nil.
  #
  #   %w[ant bear cat].any? { |word| word.length >= 3}   #=> true
  #   %w[ant bear cat].any? { |word| word.length >= 4}   #=> true
  #   [ nil, true, 99 ].any?                             #=> true

  def any?(&prc)
    prc = Proc.new { |obj| obj } unless block_given?
    each { |o| return true if prc.call(o) }
    false
  end

  ##
  # :call-seq:
  #   enum.one?                   => true or false
  #   enum.one? { |obj| block }   => true or false
  #
  # Passes each element of the collection to the given block. The method
  # returns true if the block returns true exactly once. If the block is not
  # given, one? will return true only if exactly one of the collection members
  # are true.
  #
  #   %w[ant bear cat].one? { |word| word.length == 4}   #=> true
  #   %w[ant bear cat].one? { |word| word.length >= 4}   #=> false
  #   [ nil, true, 99 ].one?                             #=> true

  def one?(&prc)
    prc = Proc.new { |obj| obj } unless block_given?
    times = 0
    each { |o| times += 1 if prc.call(o) }
    times == 1
  end

  ##
  # :call-seq:
  #   enum.none?                   => true or false
  #   enum.none? { |obj| block }   => true or false
  #
  # Passes each element of the collection to the given block. The method
  # returns true if the block never returns true for all elements. If the
  # block is not given, none? will return true only if any of the collection
  # members is true.
  #
  #    %w{ant bear cat}.none? { |word| word.length == 4}   #=> true
  #    %w{ant bear cat}.none? { |word| word.length >= 4}   #=> false
  #    [ nil, true, 99 ].none?                             #=> true

  def none?(&prc)
    prc = Proc.new { |obj| obj } unless block_given?
    times = 0
    each { |o| times += 1 if prc.call(o) }
    times == 0
  end

  ##
  # :call-seq:
  #   enum.min                    => obj
  #   enum.min { | a,b | block }  => obj
  #
  # Returns the object in +enum+ with the minimum value. The first form
  # assumes all objects implement Comparable; the second uses the block to
  # return <tt>a <=> b</tt>.
  #
  #   a = %w[albatross dog horse]
  #   a.min                                  #=> "albatross"
  #   a.min { |a,b| a.length <=> b.length }   #=> "dog"

  def min(&prc)
    prc = Proc.new { |a, b| a <=> b } unless block_given?
    min = Undefined
    each do |o|
      if min.equal? Undefined
        min = o
      else
        comp = prc.call(o, min)
        if comp.nil?
          raise ArgumentError, "comparison of #{o.class} with #{min} failed"
        elsif comp < 0
          min = o
        end
      end
    end

    min.equal?(Undefined) ? nil : min
  end

  ##
  # :call-seq:
  #   enum.max                   => obj
  #   enum.max { |a,b| block }   => obj
  #
  # Returns the object in +enum+ with the maximum value. The first form
  # assumes all objects implement Comparable; the second uses the block to
  # return <tt>a <=> b</tt>.
  #
  #    a = %w[albatross dog horse]
  #    a.max                                  #=> "horse"
  #    a.max { |a,b| a.length <=> b.length }   #=> "albatross"

  def max(&prc)
    prc = Proc.new { |a, b| a <=> b } unless block_given?
    max = Undefined
    each do |o|
      if max.equal? Undefined
        max = o
      else
        comp = prc.call(o, max)
        if comp.nil?
          raise ArgumentError, "comparison of #{o.class} with #{max} failed"
        elsif comp > 0
          max = o
        end
      end
    end

    max.equal?(Undefined) ? nil : max
  end

  ##
  # :call-seq:
  #   enum.min_by { |obj| block }   => obj
  #
  # Uses the values returned by the given block as a substitute for the real
  # object to determine what is considered the smallest object in +enum+ using
  # <tt>lhs <=> rhs</tt>. In the event of a tie, the object that appears first
  # in #each is chosen. Returns the "smallest" object or nil if the enum is
  # empty.
  #
  #   a = %w[albatross dog horse]
  #   a.min_by { |x| x.length }   #=> "dog"

  def min_by()
    min_obj, min_value = Undefined, Undefined

    each do |o|
      value = yield(o)

      if min_obj.equal?(Undefined) or (min_value <=> value) > 0
        min_obj, min_value = o, value
      end
    end

    min_obj.equal?(Undefined) ? nil : min_obj
  end

  ##
  # :call-seq:
  #   enum.max_by { | obj| block }   => obj
  #
  # Uses the values returned by the given block as a substitute for the real
  # object to determine what is considered the largest object in +enum+ using
  # <tt>lhs <=> rhs</tt>. In the event of a tie, the object that appears first
  # in #each is chosen. Returns the "largest" object or nil if the enum is
  # empty.
  #
  #   a = %w[albatross dog horse]
  #   a.max_by { |x| x.length }   #=> "albatross"

  def max_by()
    max_obj, max_value = Undefined, Undefined

    each do |o|
      value = yield(o)

      if max_obj.equal?(Undefined) or (max_value <=> value) < 0
        max_obj, max_value = o, value
      end
    end

    max_obj.equal?(Undefined) ? nil : max_obj
  end


  # :call-seq:
  #   enum.include?(obj)     => true or false
  #   enum.member?(obj)      => true or false
  #
  # Returns true if any member of +enum+ equals +obj+. Equality is tested
  # using #==.
  #
  #   IO.constants.include? "SEEK_SET"          #=> true
  #   IO.constants.include? "SEEK_NO_FURTHER"   #=> false

  def include?(obj)
    each { |o| return true if obj == o }
    false
  end

  alias_method :member?, :include?

  ##
  # :call-seq:
  #   enum.each_with_index { |obj, i| block }  -> enum
  #
  # Calls +block+ with two arguments, the item and its index, for
  # each item in +enum+.
  #
  #   hash = {}
  #   %w[cat dog wombat].each_with_index { |item, index|
  #     hash[item] = index
  #   }
  #   
  #   p hash   #=> {"cat"=>0, "wombat"=>2, "dog"=>1}

  def each_with_index
    idx = 0
    each { |o| yield(o, idx); idx += 1 }
    self
  end

  ##
  # :call-seq:
  #    enum.zip(arg, ...)                   => array
  #    enum.zip(arg, ...) { |arr| block }   => nil
  #
  # Converts any arguments to arrays, then merges elements of +enum+ with
  # corresponding elements from each argument. This generates a sequence of
  # enum#size +n+-element arrays, where +n+ is one more that the count of
  # arguments. If the size of any argument is less than enum#size, nil values
  # are supplied. If a block given, it is invoked for each output array,
  # otherwise an array of arrays is returned.
  #
  #   a = [ 4, 5, 6 ]
  #   b = [ 7, 8, 9 ]
  #
  #   (1..3).zip(a, b)      #=> [[1, 4, 7], [2, 5, 8], [3, 6, 9]]
  #   "cat\ndog".zip([1])   #=> [["cat\n", 1], ["dog", nil]]
  #   (1..3).zip            #=> [[1], [2], [3]]

  def zip(*args)
    result = []
    args = args.map { |a| a.to_a }
    each_with_index do |o, i|
      result << args.inject([o]) { |ary, a| ary << a[i] }
      yield(result.last) if block_given?
    end
    result unless block_given?
  end


end

