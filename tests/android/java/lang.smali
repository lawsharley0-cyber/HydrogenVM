.class Ltest/LangChecks; 1
.super Ljava/lang/Object;

.method longLeadingZeros(J)I 9
.registers 4
.outs 2
invoke-static Ljava/lang/Long;->numberOfLeadingZeros(J)I, v2, v3
move-result v0
return v0
.end method

.method intTrailingZeros(I)I 9
.registers 2
.outs 1
invoke-static Ljava/lang/Integer;->numberOfTrailingZeros(I)I, v1
move-result v0
return v0
.end method

.method intReverseBytes(I)I 9
.registers 2
.outs 1
invoke-static Ljava/lang/Integer;->reverseBytes(I)I, v1
move-result v0
return v0
.end method

.method parseLongRadix(Ljava/lang/String;I)J 9
.registers 4
.outs 2
invoke-static Ljava/lang/Long;->parseLong(Ljava/lang/String;I)J, v2, v3
move-result-wide v0
return-wide v0
.end method

.method longToRadix(JI)Ljava/lang/String; 9
.registers 4
.outs 3
invoke-static Ljava/lang/Long;->toString(JI)Ljava/lang/String;, v1, v2, v3
move-result-object v0
return-object v0
.end method

.method floatToBits(F)I 9
.registers 2
.outs 1
invoke-static Ljava/lang/Float;->floatToIntBits(F)I, v1
move-result v0
return v0
.end method

.method floatCompare(FF)I 9
.registers 3
.outs 2
invoke-static Ljava/lang/Float;->compare(FF)I, v1, v2
move-result v0
return v0
.end method

.method boxRoundTrip(I)J 9
.registers 4
.outs 1
invoke-static Ljava/lang/Integer;->valueOf(I)Ljava/lang/Integer;, v3
move-result-object v2
check-cast v2, Ljava/lang/Number;
invoke-virtual Ljava/lang/Number;->longValue()J, v2
move-result-wide v0
return-wide v0
.end method

.method buildGreeting(Ljava/lang/String;IZ)Ljava/lang/String; 9
.registers 5
.outs 2
new-instance v0, Ljava/lang/StringBuilder;
invoke-direct Ljava/lang/StringBuilder;-><init>()V, v0
invoke-virtual Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;, v0, v2
const/16 v1, 58
invoke-virtual Ljava/lang/StringBuilder;->append(C)Ljava/lang/StringBuilder;, v0, v1
invoke-virtual Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;, v0, v3
const/16 v1, 61
invoke-virtual Ljava/lang/StringBuilder;->append(C)Ljava/lang/StringBuilder;, v0, v1
invoke-virtual Ljava/lang/StringBuilder;->append(Z)Ljava/lang/StringBuilder;, v0, v4
invoke-virtual Ljava/lang/StringBuilder;->toString()Ljava/lang/String;, v0
move-result-object v0
return-object v0
.end method

.method stringHash(Ljava/lang/String;)I 9
.registers 2
.outs 1
invoke-virtual Ljava/lang/String;->hashCode()I, v1
move-result v0
return v0
.end method

.method stringSubstring(Ljava/lang/String;II)Ljava/lang/String; 9
.registers 4
.outs 3
invoke-virtual Ljava/lang/String;->substring(II)Ljava/lang/String;, v1, v2, v3
move-result-object v0
return-object v0
.end method

.method stringReplace(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String; 9
.registers 4
.outs 3
invoke-virtual Ljava/lang/String;->replace(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;, v1, v2, v3
move-result-object v0
return-object v0
.end method

.method overlapCopy([IIII)V 9
.registers 4
.outs 5
invoke-static Ljava/lang/System;->arraycopy(Ljava/lang/Object;ILjava/lang/Object;II)V, v0, v1, v0, v2, v3
return-void
.end method
