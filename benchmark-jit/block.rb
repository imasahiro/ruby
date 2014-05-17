def m(i)
  yield i
end

i = 0
j = 0
while i<30_000_000 # while loop 1
  i += 1
  j = m(i) {|i| i + 1}
end

puts j
