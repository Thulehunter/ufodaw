--@param count 3 How many
--@param name Hello A word
assert(params.count == 3 and type(params.count) == "number", "numbers arrive as numbers")
assert(params.name == "Hello", "text stays text")
print("params ok")
