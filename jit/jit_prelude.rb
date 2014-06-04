class Integer
  def times &block
    return to_enum :times unless block_given?
    i = 0
    while i < self
      yield i
      i += 1
    end
    self
  end
end

class Range
  def each(&block)
    return to_enum :each unless block_given?

    i = self.first
    unless i.respond_to? :succ
      raise TypeError, "can't iterate"
    end

    last = self.last
    return self if (i > last)

    while(i < last)
      yield i # FIXME `block.call` is not supported
      i = i.succ
    end

    if not exclude_end? and (i == last)
      yield i # FIXME `block.call` is not supported
    end
    self
  end
end


