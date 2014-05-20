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

j = 0
30_000_000.times do |i|
    j += 1
    #if i > 0 && i % 2 == 0
    #    break;
    #end
    #puts i
end
puts j
