.class Ltest/MathChecks; 1
.super Ljava/lang/Object;

.method absInt(I)I 9
.registers 2
.outs 1
invoke-static Ljava/lang/Math;->abs(I)I, v1
move-result v0
return v0
.end method

.method absLong(J)J 9
.registers 4
.outs 2
invoke-static Ljava/lang/Math;->abs(J)J, v2, v3
move-result-wide v0
return-wide v0
.end method

.method absFloat(F)F 9
.registers 2
.outs 1
invoke-static Ljava/lang/Math;->abs(F)F, v1
move-result v0
return v0
.end method

.method absDouble(D)D 9
.registers 4
.outs 2
invoke-static Ljava/lang/Math;->abs(D)D, v2, v3
move-result-wide v0
return-wide v0
.end method

.method minDouble(DD)D 9
.registers 6
.outs 4
invoke-static Ljava/lang/Math;->min(DD)D, v2, v3, v4, v5
move-result-wide v0
return-wide v0
.end method

.method maxDouble(DD)D 9
.registers 6
.outs 4
invoke-static Ljava/lang/Math;->max(DD)D, v2, v3, v4, v5
move-result-wide v0
return-wide v0
.end method

.method minFloat(FF)F 9
.registers 3
.outs 2
invoke-static Ljava/lang/Math;->min(FF)F, v1, v2
move-result v0
return v0
.end method

.method maxFloat(FF)F 9
.registers 3
.outs 2
invoke-static Ljava/lang/Math;->max(FF)F, v1, v2
move-result v0
return v0
.end method

.method minInt(II)I 9
.registers 3
.outs 2
invoke-static Ljava/lang/Math;->min(II)I, v1, v2
move-result v0
return v0
.end method

.method maxLong(JJ)J 9
.registers 6
.outs 4
invoke-static Ljava/lang/Math;->max(JJ)J, v2, v3, v4, v5
move-result-wide v0
return-wide v0
.end method

.method ceilDouble(D)D 9
.registers 4
.outs 2
invoke-static Ljava/lang/Math;->ceil(D)D, v2, v3
move-result-wide v0
return-wide v0
.end method

.method floorDouble(D)D 9
.registers 4
.outs 2
invoke-static Ljava/lang/Math;->floor(D)D, v2, v3
move-result-wide v0
return-wide v0
.end method

.method roundFloat(F)I 9
.registers 2
.outs 1
invoke-static Ljava/lang/Math;->round(F)I, v1
move-result v0
return v0
.end method

.method roundDouble(D)J 9
.registers 4
.outs 2
invoke-static Ljava/lang/Math;->round(D)J, v2, v3
move-result-wide v0
return-wide v0
.end method

.method signumFloat(F)F 9
.registers 2
.outs 1
invoke-static Ljava/lang/Math;->signum(F)F, v1
move-result v0
return v0
.end method

.method sqrtDouble(D)D 9
.registers 4
.outs 2
invoke-static Ljava/lang/Math;->sqrt(D)D, v2, v3
move-result-wide v0
return-wide v0
.end method

.method powDouble(DD)D 9
.registers 6
.outs 4
invoke-static Ljava/lang/Math;->pow(DD)D, v2, v3, v4, v5
move-result-wide v0
return-wide v0
.end method

.method atan2Double(DD)D 9
.registers 6
.outs 4
invoke-static Ljava/lang/Math;->atan2(DD)D, v2, v3, v4, v5
move-result-wide v0
return-wide v0
.end method

.method hypotDouble(DD)D 9
.registers 6
.outs 4
invoke-static Ljava/lang/Math;->hypot(DD)D, v2, v3, v4, v5
move-result-wide v0
return-wide v0
.end method

.method sinDouble(D)D 9
.registers 4
.outs 2
invoke-static Ljava/lang/Math;->sin(D)D, v2, v3
move-result-wide v0
return-wide v0
.end method

.method cosDouble(D)D 9
.registers 4
.outs 2
invoke-static Ljava/lang/Math;->cos(D)D, v2, v3
move-result-wide v0
return-wide v0
.end method
