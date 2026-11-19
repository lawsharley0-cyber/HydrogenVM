.class Ltest/RegexChecks; 1
.super Ljava/lang/Object;

.method compileAndMatch(Ljava/lang/String;ILjava/lang/String;)Z 9
.registers 5
.outs 2
invoke-static Ljava/util/regex/Pattern;->compile(Ljava/lang/String;I)Ljava/util/regex/Pattern;, v2, v3
move-result-object v0
invoke-virtual Ljava/util/regex/Pattern;->matcher(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;, v0, v4
move-result-object v0
invoke-virtual Ljava/util/regex/Matcher;->matches()Z, v0
move-result v1
return v1
.end method

.method findGroup(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String; 9
.registers 5
.outs 2
invoke-static Ljava/util/regex/Pattern;->compile(Ljava/lang/String;)Ljava/util/regex/Pattern;, v2
move-result-object v0
invoke-virtual Ljava/util/regex/Pattern;->matcher(Ljava/lang/CharSequence;)Ljava/util/regex/Matcher;, v0, v3
move-result-object v0
invoke-virtual Ljava/util/regex/Matcher;->find()Z, v0
move-result v1
if-eqz v1, :miss
invoke-virtual Ljava/util/regex/Matcher;->group(I)Ljava/lang/String;, v0, v4
move-result-object v0
return-object v0
:miss
const/4 v0, 0
return-object v0
.end method

.method splitString(Ljava/lang/String;Ljava/lang/String;I)[Ljava/lang/String; 9
.registers 4
.outs 3
invoke-virtual Ljava/lang/String;->split(Ljava/lang/String;I)[Ljava/lang/String;, v1, v2, v3
move-result-object v0
return-object v0
.end method

.method replaceAll(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String; 9
.registers 4
.outs 3
invoke-virtual Ljava/lang/String;->replaceAll(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;, v1, v2, v3
move-result-object v0
return-object v0
.end method

.method makeLocale(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String; 9
.registers 4
.outs 3
new-instance v0, Ljava/util/Locale;
invoke-direct Ljava/util/Locale;-><init>(Ljava/lang/String;Ljava/lang/String;)V, v0, v2, v3
invoke-virtual Ljava/util/Locale;->toString()Ljava/lang/String;, v0
move-result-object v0
return-object v0
.end method
