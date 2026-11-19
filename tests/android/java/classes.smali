# Layout fixture. Flags are the DEX access-flag bits.
.string greeting "A\u0000\u00e9\ud83d\ude00"
.class Ltest/Marker; 0x601
.method value()I 0x401
.end method

.class Ltest/Probe; 1
.implements Ltest/Marker;
.source "Probe.smali"
.field answer:I 9 = -17
.field greeting:Ljava/lang/String; 9 = greeting
.field ready:Z 9 = true
.field ticks:J 9 = 0x123456789ab
.field count:I 1

.method string()Ljava/lang/String; 9
.registers 1
const-string v0, greeting
return-object v0
.end method

.method value()I 1
.registers 2
:start
const/4 v0, 7
:end
return v0
:typed
move-exception v0
const/4 v0, 2
return v0
:all
move-exception v0
const/4 v0, 3
return v0
.catch Ljava/lang/ArithmeticException; :start :end :typed
.catch * :start :end :all
.end method

.method wide(JD)J 9
.registers 6
const-wide/16 v0, -8
add-long v0, v0, v2
return-wide v0
.end method
