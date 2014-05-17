class Integer
    def times &block
        return to_enum :times unless block_given?
        i = 0
        while i < self
            block.call i
            i += 1
        end
        self
    end
end

3.times do |i|
    #if i > 0 && i % 2 == 0
    #    break;
    #end
    puts i
end
