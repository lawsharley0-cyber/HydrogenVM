.class Ltest/Arithmetic; 1

.method wrapInt()I 9
.registers 2
const v0, 0x7fffffff
const/4 v1, 1
add-int v0, v0, v1
return v0
.end method

.method divide(II)I 9
.registers 2
div-int v0, v0, v1
return v0
.end method

.method wide(JJ)J 9
.registers 4
.words 0x009e, 0x0200
return-wide v0
.end method

.method divFloat(FF)F 9
.registers 2
.words 0x00a9, 0x0100
return v0
.end method

.method mulDouble(DD)D 9
.registers 4
.words 0x00ad, 0x0200
return-wide v0
.end method

.method cmpg(FF)I 9
.registers 2
.words 0x002e, 0x0100
return v0
.end method

.method toLong(D)J 9
.registers 2
.words 0x008b
return-wide v0
.end method

.method catchFinally()I 9
.registers 4
const/4 v0, 7
const/4 v1, 0
const/4 v2, 3
:inner
div-int v0, v0, v1
:endInner
return v0
:cleanup
move-exception v3
add-int/2addr v2, v2
:raise
throw v3
:endRaise
:outer
move-exception v3
return v2
.catch * :inner :endInner :cleanup
.catch Ljava/lang/ArithmeticException; :raise :endRaise :outer
.end method

.method throwNull()V 9
.registers 1
const/4 v0, 0
throw v0
.end method

.method packed(I)I 9
.registers 2
# Offset from packed-switch at unit 0 to its payload at unit 10.
.words 0x012b, 10, 0
const/16 v0, -1
return v0
const/16 v0, 40
return v0
nop
# One key (7) with its arm at unit 6, relative to unit 0.
.words 0x0100, 1, 7, 0, 6, 0
.end method

.method sparse(I)I 9
.registers 2
.words 0x012c, 10, 0
const/16 v0, -1
return v0
const/16 v0, 45
return v0
nop
# One signed key (-19) with its arm at unit 6, relative to unit 0.
.words 0x0200, 1, 0xffed, 0xffff, 6, 0
.end method

.method loop()V 9
.registers 0
:start
nop
goto :start
.end method
