import os
from random import randint

amount = 100
score = 0
fails = []

for i in range(1,amount + 1):
    for j in range(1,6):
        seed = j
        w = i
        t = randint(1,14)
        x = os.popen("./tester --seed " + str(seed) + " -w " + str(w) + " ./reliable").read()
        print(x, end='')
        if "14/14" in x:
            score = score + 1
        else:
            #fails.append(x[0:7] + " failed with seed: " + str(seed) + " and window: "+ str(w) + ".")
            fails.append(x + "\n seed: " + str(seed) + ", w: " + str(w))
    

print("\nScore: " + str(score) + "/" + str(amount * 5))
for f in fails:
    print(f)
    

    
