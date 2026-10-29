// HydrogenVM Foundation test app: exercises the Foundation / CoreFoundation /
// libdispatch HLE through a real ld64-linked 32-bit iOS binary.
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
extern const CFNumberRef kCFNumberNaN;
extern const CFNumberRef kCFNumberNegativeInfinity;
extern const CFNumberRef kCFNumberPositiveInfinity;


static int failures = 0, checks = 0;
#define CHECK(cond, what) do { ++checks; if (cond) printf("ok %s\n", what); else { ++failures; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)
#define CHECK_STR(a, b, what) do { NSString *_a = (a); const char *_b = (b); ++checks; \
    if (_a && strcmp([_a UTF8String], _b) == 0) printf("ok %s\n", what); \
    else { ++failures; printf("FAIL %s: got \"%s\" want \"%s\" (line %d)\n", what, _a ? [_a UTF8String] : "(nil)", _b, __LINE__); } } while (0)

static void runLoopFor(double seconds) {
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

// ------------------------------------------------------------------ model ---

@interface MyCopyKey : NSObject <NSCopying> {
@public
    int _copyCount;
}
@end
@implementation MyCopyKey
- (id)copyWithZone:(NSZone *)zone {
    MyCopyKey *k = [[MyCopyKey alloc] init];
    k->_copyCount = _copyCount + 1;
    return k;
}
@end

@interface Person : NSObject <NSCoding>
@property (nonatomic, copy) NSString *name;
@property (nonatomic) NSInteger age;
@property (nonatomic) BOOL member;
@property (nonatomic) float score;
@end
@implementation Person
- (NSString *)description { return [NSString stringWithFormat:@"<Person %@ %ld>", self.name, (long)self.age]; }
- (void)encodeWithCoder:(NSCoder *)c {
    [c encodeObject:self.name forKey:@"name"];
    [c encodeInteger:self.age forKey:@"age"];
    [c encodeBool:self.member forKey:@"member"];
    [c encodeFloat:self.score forKey:@"score"];
}
- (id)initWithCoder:(NSCoder *)c {
    if ((self = [super init])) {
        _name = [c decodeObjectForKey:@"name"];
        _age = [c decodeIntegerForKey:@"age"];
        _member = [c decodeBoolForKey:@"member"];
        _score = [c decodeFloatForKey:@"score"];
    }
    return self;
}
@end

@interface Pinger : NSObject
@property (nonatomic) int timerFires, delayed, notes, mainCalls;
@property (nonatomic, strong) NSString *lastNoteName;
@end
@implementation Pinger
- (void)tick:(NSTimer *)t { if (++_timerFires == 3) [t invalidate]; }
- (void)later:(NSString *)arg { if ([arg isEqualToString:@"x"]) _delayed++; }
- (void)note:(NSNotification *)n { _notes++; self.lastNoteName = n.name; }
- (void)onMain:(id)arg { if ([NSThread isMainThread]) _mainCalls++; }
- (void)background:(id)arg {
    @autoreleasepool {
        [self performSelectorOnMainThread:@selector(onMain:) withObject:nil waitUntilDone:YES];
    }
}
@end

static void timerCallback(CFRunLoopTimerRef t, void *info) { (*(int *)info)++; }

// ------------------------------------------------------------------ tests ---

static void testStrings(void) {
    NSString *s = [NSString stringWithFormat:@"%d-%@-%.2f-%s-%C", 42, @"obj", 3.14159, "c", (unsigned short)0x263A];
    CHECK_STR(s, "42-obj-3.14-c-\xE2\x98\xBA", "stringWithFormat");
    CHECK([s length] == 15, "length counts UTF-16 units");
    CHECK_STR([@"Hello World" substringWithRange:NSMakeRange(6, 5)], "World", "substringWithRange");
    CHECK([@"Hello" compare:@"hello" options:NSCaseInsensitiveSearch] == NSOrderedSame, "case-insensitive compare");
    CHECK([@"file10" compare:@"file9" options:NSNumericSearch] == NSOrderedDescending, "numeric compare");
    NSArray *parts = [@"a,b,,c" componentsSeparatedByString:@","];
    CHECK([parts count] == 4 && [parts[3] isEqualToString:@"c"], "componentsSeparatedByString");
    CHECK_STR([parts componentsJoinedByString:@"|"], "a|b||c", "componentsJoinedByString");
    CHECK_STR([@"  trim me \n" stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]], "trim me", "trimming");
    CHECK_STR([@"/var/mobile/Documents/save.dat" lastPathComponent], "save.dat", "lastPathComponent");
    CHECK_STR([@"save.dat" pathExtension], "dat", "pathExtension");
    CHECK_STR([@"/a/b" stringByAppendingPathComponent:@"c.png"], "/a/b/c.png", "stringByAppendingPathComponent");
    CHECK_STR([@"/a/b/c.png" stringByDeletingPathExtension], "/a/b/c", "stringByDeletingPathExtension");
    CHECK_STR([@"abc" stringByReplacingOccurrencesOfString:@"b" withString:@"XX"], "aXXc", "replaceOccurrences");
    CHECK([@"123" intValue] == 123 && [@"2.5" floatValue] == 2.5f && [@"YES" boolValue], "numeric parsing");
    NSMutableString *m = [NSMutableString stringWithCapacity:4];
    [m appendString:@"ab"];
    [m appendFormat:@"%03d", 7];
    [m insertString:@"_" atIndex:1];
    [m deleteCharactersInRange:NSMakeRange(0, 1)];
    CHECK_STR(m, "_b007", "NSMutableString edits");
    CHECK_STR([@"MiXeD" lowercaseString], "mixed", "lowercaseString");
    CHECK_STR([@"hello world" capitalizedString], "Hello World", "capitalizedString");
    NSRange r = [@"find the needle" rangeOfString:@"needle"];
    CHECK(r.location == 9 && r.length == 6, "rangeOfString");
    CHECK([@"nope" rangeOfString:@"x"].location == NSNotFound, "rangeOfString not found");
    NSData *utf8 = [@"h\xC3\xA9" dataUsingEncoding:NSUTF8StringEncoding];
    CHECK([utf8 length] == 3, "dataUsingEncoding UTF-8");
    NSString *back = [[NSString alloc] initWithData:utf8 encoding:NSUTF8StringEncoding];
    CHECK([back length] == 2 && [back characterAtIndex:1] == 0xE9, "initWithData round trip");
    CHECK_STR([@"a b&c" stringByAddingPercentEscapesUsingEncoding:NSUTF8StringEncoding], "a%20b&c", "percent escapes");
    CHECK_STR(NSStringFromClass([Person class]), "Person", "NSStringFromClass");
    CHECK(NSClassFromString(@"NSArray") == [NSArray class], "NSClassFromString");
    CHECK_STR(NSStringFromSelector(@selector(tick:)), "tick:", "NSStringFromSelector");
    CHECK_STR(NSStringFromRange(NSMakeRange(3, 4)), "{3, 4}", "NSStringFromRange");
}

static void testNumbersAndCollections(void) {
    NSNumber *n = @42, *f = @2.5, *b = @YES;
    CHECK([n intValue] == 42 && [f doubleValue] == 2.5 && [b boolValue], "NSNumber literals");
    CHECK([@1 compare:@2] == NSOrderedAscending && [@3 isEqualToNumber:@3.0], "NSNumber compare");
    CHECK_STR([f stringValue], "2.5", "NSNumber stringValue");

    NSArray *a = @[@"pear", @"apple", @"fig"];
    NSArray *sorted = [a sortedArrayUsingSelector:@selector(compare:)];
    CHECK_STR([sorted componentsJoinedByString:@","], "apple,fig,pear", "sortedArrayUsingSelector");
    NSArray *byLen = [a sortedArrayUsingComparator:^NSComparisonResult(NSString *x, NSString *y) {
        return x.length < y.length ? NSOrderedAscending : x.length > y.length ? NSOrderedDescending : NSOrderedSame;
    }];
    CHECK_STR([byLen componentsJoinedByString:@","], "fig,pear,apple", "sortedArrayUsingComparator");
    NSMutableString *joined = [NSMutableString string];
    for (NSString *s in a) [joined appendString:[s substringToIndex:1]];
    CHECK_STR(joined, "paf", "fast enumeration");
    __block NSUInteger sumIdx = 0;
    [a enumerateObjectsUsingBlock:^(id obj, NSUInteger idx, BOOL *stop) { sumIdx += idx; if (idx == 1) *stop = YES; }];
    CHECK(sumIdx == 1, "enumerateObjectsUsingBlock stop");
    CHECK([a indexOfObject:@"fig"] == 2 && [a containsObject:@"apple"], "indexOfObject / containsObject");
    NSMutableArray *ma = [a mutableCopy];
    [ma removeObjectAtIndex:0];
    [ma addObject:@"kiwi"];
    ma[0] = @"APPLE";
    CHECK_STR([ma componentsJoinedByString:@","], "APPLE,fig,kiwi", "NSMutableArray edits");

    NSDictionary *d = @{@"one": @1, @"two": @2, @"nested": @{@"k": @[@"v"]}};
    CHECK([d[@"two"] intValue] == 2 && [d count] == 3, "dictionary literal");
    CHECK_STR([d valueForKeyPath:@"nested.k"][0], "v", "valueForKeyPath");
    __block int total = 0;
    [d enumerateKeysAndObjectsUsingBlock:^(id key, id obj, BOOL *stop) { if ([obj isKindOfClass:[NSNumber class]]) total += [obj intValue]; }];
    CHECK(total == 3, "enumerateKeysAndObjects");
    NSMutableDictionary *md = [NSMutableDictionary dictionary];
    md[@"a"] = @"x";
    [md setObject:@"y" forKey:@"b"];
    [md removeObjectForKey:@"a"];
    CHECK([md count] == 1 && [md[@"b"] isEqualToString:@"y"], "NSMutableDictionary edits");
    CHECK(([[[d allKeys] sortedArrayUsingSelector:@selector(compare:)] isEqualToArray:@[@"nested", @"one", @"two"]]), "allKeys");

    NSSet *set = [NSSet setWithObjects:@"a", @"b", @"a", nil];
    CHECK([set count] == 2 && [set containsObject:@"b"], "NSSet dedup");
    NSIndexSet *is = [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(2, 3)];
    CHECK([is count] == 3 && [is firstIndex] == 2 && [is lastIndex] == 4 && ![is containsIndex:5], "NSIndexSet");

    NSArray *orig = @[@"A", @"B", @"C", @"D", @"E"];
    NSMutableIndexSet *is13 = [NSMutableIndexSet indexSet];
    [is13 addIndex:1];
    [is13 addIndex:3];

    NSArray *sub = [orig objectsAtIndexes:is13];
    CHECK_STR([sub componentsJoinedByString:@","], "B,D", "NSArray objectsAtIndexes:");

    BOOL caughtOOB = NO;
    @try {
        NSMutableIndexSet *badIS = [NSMutableIndexSet indexSetWithIndex:10];
        (void)[orig objectsAtIndexes:badIS];
    } @catch (NSException *e) {
        caughtOOB = [e.name isEqualToString:@"NSRangeException"];
    }
    CHECK(caughtOOB, "objectsAtIndexes: out-of-bounds raises NSRangeException");

    NSMutableArray *maIndex = [orig mutableCopy];
    [maIndex removeObjectsAtIndexes:is13];
    CHECK_STR([maIndex componentsJoinedByString:@","], "A,C,E", "NSMutableArray removeObjectsAtIndexes:");

    caughtOOB = NO;
    @try {
        [maIndex removeObjectsAtIndexes:[NSIndexSet indexSetWithIndex:10]];
    } @catch (NSException *e) {
        caughtOOB = [e.name isEqualToString:@"NSRangeException"];
    }
    CHECK(caughtOOB, "removeObjectsAtIndexes: out-of-bounds raises NSRangeException");

    NSMutableArray *maIns = [@[@"A", @"B"] mutableCopy];
    [maIns insertObjects:@[@"X", @"Y"] atIndexes:is13];
    CHECK_STR([maIns componentsJoinedByString:@","], "A,X,B,Y", "NSMutableArray insertObjects:atIndexes:");

    BOOL caughtMismatch = NO;
    @try {
        [maIns insertObjects:@[@"X"] atIndexes:is13];
    } @catch (NSException *e) {
        caughtMismatch = YES;
    }
    CHECK(caughtMismatch, "insertObjects:atIndexes: count mismatch raises exception");

    caughtOOB = NO;
    @try {
        [maIns insertObjects:@[@"X"] atIndexes:[NSIndexSet indexSetWithIndex:100]];
    } @catch (NSException *e) {
        caughtOOB = [e.name isEqualToString:@"NSRangeException"];
    }
    CHECK(caughtOOB, "insertObjects:atIndexes: out-of-bounds raises NSRangeException");

    NSMutableArray *maRepl = [@[@"A", @"B", @"C", @"D"] mutableCopy];
    [maRepl replaceObjectsAtIndexes:is13 withObjects:@[@"X", @"Y"]];
    CHECK_STR([maRepl componentsJoinedByString:@","], "A,X,C,Y", "NSMutableArray replaceObjectsAtIndexes:withObjects:");

    caughtMismatch = NO;
    @try {
        [maRepl replaceObjectsAtIndexes:is13 withObjects:@[@"X"]];
    } @catch (NSException *e) {
        caughtMismatch = YES;
    }
    CHECK(caughtMismatch, "replaceObjectsAtIndexes:withObjects: count mismatch raises exception");

    caughtOOB = NO;
    @try {
        [maRepl replaceObjectsAtIndexes:[NSIndexSet indexSetWithIndex:10] withObjects:@[@"X"]];
    } @catch (NSException *e) {
        caughtOOB = [e.name isEqualToString:@"NSRangeException"];
    }
    CHECK(caughtOOB, "replaceObjectsAtIndexes:withObjects: out-of-bounds raises NSRangeException");

    NSArray *words = @[@"apple", @"pear", @"fig", @"banana"];
    NSIndexSet *passed = [words indexesOfObjectsPassingTest:^BOOL(NSString *obj, NSUInteger idx, BOOL *stop) {
        return obj.length > 4;
    }];
    CHECK([passed count] == 2 && [passed containsIndex:0] && [passed containsIndex:3], "indexesOfObjectsPassingTest:");

    __block NSUInteger stopCount = 0;
    NSIndexSet *stopped = [words indexesOfObjectsPassingTest:^BOOL(NSString *obj, NSUInteger idx, BOOL *stop) {
        stopCount++;
        if (idx == 1) *stop = YES;
        return obj.length >= 4;
    }];
    CHECK(stopCount == 2 && [stopped count] == 2 && [stopped containsIndex:0] && [stopped containsIndex:1], "indexesOfObjectsPassingTest: stop block");

    NSString *desc = [@[@1, @"two"] description];
    CHECK([desc rangeOfString:@"two"].location != NSNotFound, "array description");
    NSMutableData *data = [NSMutableData dataWithLength:2];
    [data appendBytes:"xyz" length:3];
    CHECK([data length] == 5 && memcmp((const char *)[data bytes] + 2, "xyz", 3) == 0, "NSMutableData");
    CHECK([[NSNull null] isEqual:[NSNull null]], "NSNull singleton");
}

static void testKVCAndArchiving(void) {
    Person *p = [Person new];
    [p setValue:@"Ada" forKey:@"name"];
    [p setValue:@36 forKey:@"age"];
    [p setValue:@YES forKey:@"member"];
    [p setValue:@1.5 forKey:@"score"];
    CHECK([p.name isEqualToString:@"Ada"] && p.age == 36 && p.member && p.score == 1.5f, "KVC setValue:forKey:");
    CHECK([[p valueForKey:@"age"] intValue] == 36 && [[p valueForKey:@"score"] floatValue] == 1.5f, "KVC valueForKey:");
    NSArray *people = @[p, p];
    CHECK([[people valueForKeyPath:@"@sum.age"] intValue] == 72, "KVC collection operator");

    NSData *archive = [NSKeyedArchiver archivedDataWithRootObject:@{@"people": people, @"title": @"club"}];
    CHECK(archive != nil && [archive length] > 0, "NSKeyedArchiver archivedData");
    NSDictionary *round = [NSKeyedUnarchiver unarchiveObjectWithData:archive];
    Person *q = round[@"people"][1];
    CHECK([round[@"title"] isEqualToString:@"club"] && [q.name isEqualToString:@"Ada"] && q.age == 36 && q.member && q.score == 1.5f,
          "NSKeyedUnarchiver round trip");

    NSError *err = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:@{@"a": @[@1, @2.5, @"s", [NSNull null]], @"b": @YES} options:0 error:&err];
    CHECK(json != nil && err == nil, "NSJSONSerialization write");
    id parsed = [NSJSONSerialization JSONObjectWithData:[@"{\"x\":[1,2,{\"y\":\"z\\u00e9\"}],\"t\":true,\"n\":null,\"f\":-1.5e2}" dataUsingEncoding:NSUTF8StringEncoding]
                                                options:0 error:&err];
    CHECK([parsed[@"x"][2][@"y"] isEqualToString:@"z\xC3\xA9"] && [parsed[@"t"] boolValue] && parsed[@"n"] == [NSNull null] && [parsed[@"f"] doubleValue] == -150.0,
          "NSJSONSerialization parse");
    id reparsed = [NSJSONSerialization JSONObjectWithData:json options:0 error:&err];
    CHECK([reparsed[@"a"][1] doubleValue] == 2.5 && [reparsed[@"b"] boolValue], "JSON round trip");

    NSData *plist = [NSPropertyListSerialization dataWithPropertyList:@{@"k": @[@1, @"v"]} format:NSPropertyListXMLFormat_v1_0 options:0 error:&err];
    id plistBack = [NSPropertyListSerialization propertyListWithData:plist options:0 format:NULL error:&err];
    CHECK([plistBack[@"k"][1] isEqualToString:@"v"], "NSPropertyListSerialization");
}

static void testFilesAndBundle(void) {
    NSBundle *mb = [NSBundle mainBundle];
    CHECK_STR([mb bundleIdentifier], "com.hydrogenvm.test.foundationtest", "bundleIdentifier");
    CHECK_STR([mb objectForInfoDictionaryKey:@"CFBundleName"], "FoundationTest", "objectForInfoDictionaryKey");
    NSString *cfgPath = [mb pathForResource:@"config" ofType:@"plist"];
    CHECK(cfgPath != nil, "pathForResource");
    NSDictionary *cfg = [NSDictionary dictionaryWithContentsOfFile:cfgPath];
    CHECK([cfg[@"level"] intValue] == 7 && [cfg[@"name"] isEqualToString:@"forest"], "dictionaryWithContentsOfFile");
    CHECK_STR(NSLocalizedString(@"GREETING", nil), "Hello from en.lproj", "NSLocalizedString");
    CHECK_STR(NSLocalizedString(@"MISSING_KEY", nil), "MISSING_KEY", "NSLocalizedString fallback");

    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    CHECK([docs hasSuffix:@"/Documents"], "NSSearchPathForDirectoriesInDomains");
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dir = [docs stringByAppendingPathComponent:@"saves/slot1"];
    [fm removeItemAtPath:[docs stringByAppendingPathComponent:@"saves"] error:NULL];
    CHECK([fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL], "createDirectoryAtPath");
    NSString *file = [dir stringByAppendingPathComponent:@"state.txt"];
    NSError *err = nil;
    CHECK([@"level=3" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:&err], "writeToFile");
    CHECK_STR([NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:NULL], "level=3", "stringWithContentsOfFile");

    NSURL *fileURL = [NSURL fileURLWithPath:file];
    CHECK_STR([NSString stringWithContentsOfURL:fileURL encoding:NSUTF8StringEncoding error:NULL], "level=3", "stringWithContentsOfURL:encoding:error:");
    CHECK_STR([NSString stringWithContentsOfURL:fileURL], "level=3", "stringWithContentsOfURL:");
    CHECK_STR([[NSString alloc] initWithContentsOfURL:fileURL encoding:NSUTF8StringEncoding error:NULL], "level=3", "initWithContentsOfURL:encoding:error:");
    NSStringEncoding usedEnc = 0;
    CHECK_STR([[NSString alloc] initWithContentsOfURL:fileURL usedEncoding:&usedEnc error:NULL], "level=3", "initWithContentsOfURL:usedEncoding:error:");
    CHECK(usedEnc == NSUTF8StringEncoding, "initWithContentsOfURL:usedEncoding:error: sets usedEnc");
    CHECK_STR([[NSString alloc] initWithContentsOfURL:fileURL], "level=3", "initWithContentsOfURL:");

    NSURL *missingURL = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:@"missing.txt"]];
    CHECK([NSString stringWithContentsOfURL:missingURL encoding:NSUTF8StringEncoding error:NULL] == nil, "stringWithContentsOfURL missing file returns nil");
    CHECK([[NSString alloc] initWithContentsOfURL:missingURL encoding:NSUTF8StringEncoding error:NULL] == nil, "initWithContentsOfURL missing file returns nil");

    NSURL *remoteURL = [NSURL URLWithString:@"https://example.com/test.txt"];
    CHECK([NSString stringWithContentsOfURL:remoteURL encoding:NSUTF8StringEncoding error:NULL] == nil, "stringWithContentsOfURL remote URL returns nil");
    CHECK([[NSString alloc] initWithContentsOfURL:remoteURL] == nil, "initWithContentsOfURL remote URL returns nil");

    CHECK([NSString stringWithContentsOfURL:nil encoding:NSUTF8StringEncoding error:NULL] == nil, "stringWithContentsOfURL nil returns nil");
    CHECK([[NSString alloc] initWithContentsOfURL:nil] == nil, "initWithContentsOfURL nil returns nil");

    BOOL isDir = NO;
    CHECK([fm fileExistsAtPath:dir isDirectory:&isDir] && isDir, "fileExistsAtPath:isDirectory:");
    CHECK(([[fm contentsOfDirectoryAtPath:dir error:NULL] isEqualToArray:@[@"state.txt"]]), "contentsOfDirectoryAtPath");
    CHECK([[fm attributesOfItemAtPath:file error:NULL][NSFileSize] intValue] == 7, "attributesOfItemAtPath");
    CHECK(![fm removeItemAtPath:[dir stringByAppendingPathComponent:@"nope"] error:&err] && err != nil && [err code] == 4, "remove missing file error");
    NSArray *arr = [NSArray arrayWithObjects:@"x", @3, nil];
    NSString *arrFile = [docs stringByAppendingPathComponent:@"arr.plist"];
    CHECK([arr writeToFile:arrFile atomically:YES] && [[NSArray arrayWithContentsOfFile:arrFile] isEqualToArray:arr], "array plist file round trip");

    NSURL *u = [NSURL URLWithString:@"https://user@example.com:8080/a/b.json?q=1#frag"];
    CHECK_STR([u scheme], "https", "NSURL scheme");
    CHECK_STR([u host], "example.com", "NSURL host");
    CHECK([[u port] intValue] == 8080, "NSURL port");
    CHECK_STR([u path], "/a/b.json", "NSURL path");
    CHECK_STR([u query], "q=1", "NSURL query");
    CHECK_STR([u lastPathComponent], "b.json", "NSURL lastPathComponent");
    NSURL *fu = [NSURL fileURLWithPath:@"/tmp/my file.txt"];
    CHECK([fu isFileURL] && [[fu absoluteString] isEqualToString:@"file:///tmp/my%20file.txt"], "fileURLWithPath");
    CHECK_STR([fu path], "/tmp/my file.txt", "file URL path");
    CHECK([mb URLForResource:@"config" withExtension:@"plist"] != nil, "URLForResource");

    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    [ud registerDefaults:@{@"volume": @0.8, @"name": @"anon"}];
    CHECK([ud floatForKey:@"volume"] == 0.8f && [[ud stringForKey:@"name"] isEqualToString:@"anon"], "registerDefaults");
    [ud setInteger:12 forKey:@"highscore"];
    [ud setBool:YES forKey:@"sound"];
    [ud setObject:@[@"a"] forKey:@"list"];
    CHECK([ud integerForKey:@"highscore"] == 12 && [ud boolForKey:@"sound"] && [[ud objectForKey:@"list"] count] == 1, "NSUserDefaults values");
    CHECK([ud synchronize], "NSUserDefaults synchronize");
    NSString *prefs = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Preferences/com.hydrogenvm.test.foundationtest.plist"];
    CHECK([[NSDictionary dictionaryWithContentsOfFile:prefs][@"highscore"] intValue] == 12, "defaults persisted to plist");
    CHECK([NSTemporaryDirectory() hasSuffix:@"/tmp/"], "NSTemporaryDirectory");
}

static void testCoreFoundation(void) {
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, "core", kCFStringEncodingUTF8);
    CHECK(CFStringGetLength(s) == 4 && [(__bridge NSString *)s isEqualToString:@"core"], "CFStringCreateWithCString (bridged)");
    CFMutableStringRef ms = CFStringCreateMutableCopy(NULL, 0, s);
    CFStringAppend(ms, CFSTR("-foundation"));
    char buf[64];
    CHECK(CFStringGetCString(ms, buf, sizeof buf, kCFStringEncodingUTF8) && strcmp(buf, "core-foundation") == 0, "CFStringAppend / GetCString");
    CFRange fr = CFStringFind(ms, CFSTR("found"), 0);
    CHECK(fr.location == 5 && fr.length == 5, "CFStringFind");
    CHECK(CFStringCompare(CFSTR("ABC"), CFSTR("abc"), kCFCompareCaseInsensitive) == kCFCompareEqualTo, "CFStringCompare");
    CFStringRef fmt = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d/%@"), 5, s);
    CHECK([(__bridge NSString *)fmt isEqualToString:@"5/core"], "CFStringCreateWithFormat");
    int v = 77, out = 0;
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CHECK(CFNumberGetValue(num, kCFNumberSInt32Type, &out) && out == 77 && [(__bridge NSNumber *)num intValue] == 77, "CFNumber");

    double out_d;
    CHECK(CFNumberGetValue(kCFNumberNaN, kCFNumberDoubleType, &out_d) && (out_d != out_d), "kCFNumberNaN");
    CHECK(CFNumberGetValue(kCFNumberNegativeInfinity, kCFNumberDoubleType, &out_d) && out_d < -1e300, "kCFNumberNegativeInfinity");
    CHECK(CFNumberGetValue(kCFNumberPositiveInfinity, kCFNumberDoubleType, &out_d) && out_d > 1e300, "kCFNumberPositiveInfinity");
    CHECK(CFBooleanGetValue(kCFBooleanTrue) && (__bridge id)kCFBooleanTrue == (id)@YES, "kCFBooleanTrue is @YES");
    const void *vals[] = {s, num};
    CFArrayRef arr = CFArrayCreate(NULL, vals, 2, &kCFTypeArrayCallBacks);
    CHECK(CFArrayGetCount(arr) == 2 && CFArrayGetValueAtIndex(arr, 1) == num && [(__bridge NSArray *)arr count] == 2, "CFArray (objects)");
    static int a1 = 1, a2 = 2;
    CFMutableArrayRef raw = CFArrayCreateMutable(NULL, 0, NULL);
    CFArrayAppendValue(raw, &a1);
    CFArrayAppendValue(raw, &a2);
    CHECK(CFArrayGetCount(raw) == 2 && *(const int *)CFArrayGetValueAtIndex(raw, 1) == 2, "CFArray (raw pointers)");
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(dict, CFSTR("k"), num);
    CHECK(CFDictionaryGetCount(dict) == 1 && CFDictionaryGetValue(dict, CFSTR("k")) == num && [(__bridge NSDictionary *)dict objectForKey:@"k"] != nil,
          "CFDictionary");
    CFMutableDictionaryRef rawd = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    CFDictionarySetValue(rawd, (const void *)5, &a2);
    const void *got = NULL;
    CHECK(CFDictionaryGetValueIfPresent(rawd, (const void *)5, &got) && got == &a2 && !CFDictionaryGetValue(rawd, (const void *)6), "CFDictionary (raw)");
    CHECK(CFGetTypeID(s) == CFStringGetTypeID() && CFGetTypeID(arr) == CFArrayGetTypeID(), "CFGetTypeID");
    CFUUIDRef uuid = CFUUIDCreate(NULL);
    CFStringRef us = CFUUIDCreateString(NULL, uuid);
    CHECK(CFStringGetLength(us) == 36, "CFUUID");
    CHECK(strcmp(CFStringGetCStringPtr(CFBundleGetIdentifier(CFBundleGetMainBundle()), kCFStringEncodingUTF8), "com.hydrogenvm.test.foundationtest") == 0,
          "CFBundleGetIdentifier");
    CFURLRef cfg = CFBundleCopyResourceURL(CFBundleGetMainBundle(), CFSTR("config"), CFSTR("plist"), NULL);
    uint8_t path[512];
    CHECK(cfg && CFURLGetFileSystemRepresentation(cfg, true, path, sizeof path) && strstr((char *)path, "config.plist"), "CFBundleCopyResourceURL");
    CFPreferencesSetAppValue(CFSTR("cfpref"), CFSTR("yes"), kCFPreferencesCurrentApplication);
    CFTypeRef pv = CFPreferencesCopyAppValue(CFSTR("cfpref"), kCFPreferencesCurrentApplication);
    CHECK(pv && [(__bridge NSString *)pv isEqualToString:@"yes"], "CFPreferences");
    CFRelease(s); CFRelease(ms); CFRelease(fmt); CFRelease(num); CFRelease(arr); CFRelease(raw); CFRelease(dict); CFRelease(rawd);
    CFRelease(uuid); CFRelease(us); CFRelease(cfg); CFRelease(pv);

    MyCopyKey *copyKey = [[MyCopyKey alloc] init];
    MyCopyKey *valObj = [[MyCopyKey alloc] init];
    long initialValRc = CFGetRetainCount((__bridge CFTypeRef)valObj);
    CFMutableDictionaryRef cmd = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    NSMutableDictionary *bmd = (__bridge NSMutableDictionary *)cmd;

    [bmd setObject:valObj forKey:copyKey];

    const void *rawKeys[1] = {NULL};
    CFDictionaryGetKeysAndValues(cmd, rawKeys, NULL);
    long copiedKeyRc = CFGetRetainCount(rawKeys[0]);
    long afterValRc = CFGetRetainCount((__bridge CFTypeRef)valObj);
    CHECK(((__bridge MyCopyKey *)rawKeys[0])->_copyCount == 1 && copiedKeyRc == 1 && afterValRc == initialValRc + 1, "rd_setObjectForKey copies key, retains key copy and value");

    [bmd removeAllObjects];
    copyKey->_copyCount = 0;

    long initialKeyRc = CFGetRetainCount((__bridge CFTypeRef)copyKey);
    long initialValRc2 = CFGetRetainCount((__bridge CFTypeRef)valObj);
    CFDictionarySetValue(cmd, (__bridge const void *)copyKey, (__bridge const void *)valObj);

    const void *rawKeys2[1] = {NULL};
    CFDictionaryGetKeysAndValues(cmd, rawKeys2, NULL);
    long afterKeyRc = CFGetRetainCount((__bridge CFTypeRef)copyKey);
    long afterValRc2 = CFGetRetainCount((__bridge CFTypeRef)valObj);
    CHECK(rawKeys2[0] == (__bridge const void *)copyKey && copyKey->_copyCount == 0 && afterKeyRc == initialKeyRc + 1 && afterValRc2 == initialValRc2 + 1, "CFDictionarySetValue only retains key");
    CFRelease(cmd);
}

static dispatch_once_t onceToken;
static int onceCount = 0;

static void testConcurrency(void) {
    Pinger *p = [Pinger new];
    [NSTimer scheduledTimerWithTimeInterval:0.02 target:p selector:@selector(tick:) userInfo:nil repeats:YES];
    [p performSelector:@selector(later:) withObject:@"x" afterDelay:0.01];
    [p performSelector:@selector(later:) withObject:@"x" afterDelay:0.01];
    [NSObject cancelPreviousPerformRequestsWithTarget:p selector:@selector(later:) object:@"x"];
    [p performSelector:@selector(later:) withObject:@"x" afterDelay:0.03];
    int cfFires = 0;
    CFRunLoopTimerContext ctx = {0, &cfFires, NULL, NULL, NULL};
    CFRunLoopTimerRef cft = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.01, 0, 0, 0, timerCallback, &ctx);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), cft, kCFRunLoopDefaultMode);
    for (int i = 0; i < 50 && (p.timerFires < 3 || cfFires == 0 || p.delayed == 0); i++) runLoopFor(0.02);
    runLoopFor(0.05);
    CHECK(p.timerFires == 3, "NSTimer repeats then invalidates");
    CHECK(p.delayed == 1, "performSelector:afterDelay: + cancel");
    CHECK(cfFires == 1, "CFRunLoopTimer");
    CFRelease(cft);

    [[NSNotificationCenter defaultCenter] addObserver:p selector:@selector(note:) name:@"Ping" object:nil];
    __block int blockNotes = 0;
    id token = [[NSNotificationCenter defaultCenter] addObserverForName:@"Ping" object:nil queue:nil usingBlock:^(NSNotification *n) {
        blockNotes += [n.userInfo[@"n"] intValue];
    }];
    [[NSNotificationCenter defaultCenter] postNotificationName:@"Ping" object:nil userInfo:@{@"n": @5}];
    [[NSNotificationCenter defaultCenter] removeObserver:p];
    [[NSNotificationCenter defaultCenter] postNotificationName:@"Ping" object:nil userInfo:@{@"n": @1}];
    [[NSNotificationCenter defaultCenter] removeObserver:token];
    [[NSNotificationCenter defaultCenter] postNotificationName:@"Ping" object:nil userInfo:@{@"n": @100}];
    CHECK(p.notes == 1 && [p.lastNoteName isEqualToString:@"Ping"] && blockNotes == 6, "NSNotificationCenter");

    for (int i = 0; i < 3; i++) dispatch_once(&onceToken, ^{ onceCount++; });
    CHECK(onceCount == 1, "dispatch_once");

    __block int sum = 0;
    dispatch_queue_t serial = dispatch_queue_create("com.test.serial", DISPATCH_QUEUE_SERIAL);
    dispatch_group_t group = dispatch_group_create();
    for (int i = 1; i <= 100; i++) dispatch_group_async(group, serial, ^{ sum += i; });
    CHECK(dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0 && sum == 5050, "serial queue + group wait");
    CHECK(strcmp(dispatch_queue_get_label(serial), "com.test.serial") == 0, "dispatch_queue_get_label");

    __block int concurrentSum = 0;
    NSLock *lock = [NSLock new];
    dispatch_queue_t global = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    dispatch_apply(10, global, ^(size_t i) { [lock lock]; concurrentSum += (int)i; [lock unlock]; });
    CHECK(concurrentSum == 45, "dispatch_apply");

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block BOOL onWorker = NO;
    dispatch_async(global, ^{ onWorker = ![NSThread isMainThread]; dispatch_semaphore_signal(sem); });
    CHECK(dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0 && onWorker, "global queue runs off main + semaphore");
    CHECK(dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC)) != 0, "semaphore timeout");

    __block int syncVal = 0;
    dispatch_sync(serial, ^{ syncVal = 9; });
    CHECK(syncVal == 9, "dispatch_sync");

    dispatch_queue_t conc = dispatch_queue_create("com.test.conc", DISPATCH_QUEUE_CONCURRENT);
    __block int readers = 0, barrierSaw = -1;
    for (int i = 0; i < 4; i++) dispatch_async(conc, ^{ [lock lock]; readers++; [lock unlock]; });
    dispatch_barrier_async(conc, ^{ barrierSaw = readers; });
    dispatch_barrier_sync(conc, ^{});
    CHECK(barrierSaw == 4, "barrier waits for earlier blocks");

    __block BOOL mainAsyncRan = NO, afterRan = NO, bgToMain = NO;
    dispatch_async(dispatch_get_main_queue(), ^{ mainAsyncRan = [NSThread isMainThread]; });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ afterRan = YES; });
    dispatch_async(global, ^{ dispatch_sync(dispatch_get_main_queue(), ^{ bgToMain = [NSThread isMainThread]; }); });
    __block int notified = 0;
    dispatch_group_t g2 = dispatch_group_create();
    dispatch_group_enter(g2);
    dispatch_group_notify(g2, dispatch_get_main_queue(), ^{ notified++; });
    dispatch_async(global, ^{ dispatch_group_leave(g2); });
    runLoopFor(0.2);
    CHECK(mainAsyncRan, "dispatch_async to main queue");
    CHECK(afterRan, "dispatch_after on main queue");
    CHECK(bgToMain, "dispatch_sync to main from background");
    CHECK(notified == 1, "dispatch_group_notify after enter/leave");

    __block unsigned long ticks = 0;
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), 10 * NSEC_PER_MSEC, 0);
    dispatch_source_set_event_handler(timer, ^{ ticks += dispatch_source_get_data(timer); if (ticks >= 3) dispatch_source_cancel(timer); });
    dispatch_resume(timer);
    runLoopFor(0.2);
    CHECK(ticks >= 3, "dispatch timer source");

    NSOperationQueue *oq = [NSOperationQueue new];
    __block NSMutableArray *order = [NSMutableArray array];
    NSBlockOperation *first = [NSBlockOperation blockOperationWithBlock:^{ [NSThread sleepForTimeInterval:0.02]; @synchronized (order) { [order addObject:@"first"]; } }];
    NSBlockOperation *second = [NSBlockOperation blockOperationWithBlock:^{ @synchronized (order) { [order addObject:@"second"]; } }];
    [second addDependency:first];
    __block BOOL completed = NO;
    [second setCompletionBlock:^{ completed = YES; }];
    [oq addOperation:second];
    [oq addOperation:first];
    [oq waitUntilAllOperationsAreFinished];
    CHECK([[order componentsJoinedByString:@","] isEqualToString:@"first,second"] && completed, "NSOperationQueue dependencies");
    __block BOOL mainOp = NO;
    [[NSOperationQueue mainQueue] addOperationWithBlock:^{ mainOp = [NSThread isMainThread]; }];
    runLoopFor(0.05);
    CHECK(mainOp, "NSOperationQueue mainQueue");

    [NSThread detachNewThreadSelector:@selector(background:) toTarget:p withObject:nil];
    for (int i = 0; i < 100 && p.mainCalls == 0; i++) runLoopFor(0.01);
    CHECK(p.mainCalls == 1, "NSThread + performSelectorOnMainThread:waitUntilDone:");
}

static void testMisc(void) {
    NSError *e = [NSError errorWithDomain:NSCocoaErrorDomain code:3 userInfo:@{NSLocalizedDescriptionKey: @"bad thing"}];
    CHECK([e code] == 3 && [[e localizedDescription] isEqualToString:@"bad thing"], "NSError");
    NSDate *d1 = [NSDate date];
    NSDate *d2 = [d1 dateByAddingTimeInterval:60];
    CHECK([d2 timeIntervalSinceDate:d1] == 60 && [d1 compare:d2] == NSOrderedAscending, "NSDate arithmetic");
    CHECK([d1 timeIntervalSince1970] > 1.5e9, "NSDate is current");
    CHECK([[[NSUUID UUID] UUIDString] length] == 36, "NSUUID");
    CHECK([[[NSProcessInfo processInfo] processName] isEqualToString:@"FoundationTest"], "NSProcessInfo processName");
    NSIndexPath *ip = [NSIndexPath indexPathForRow:3 inSection:1];
    CHECK([ip row] == 3 && [ip section] == 1, "NSIndexPath");
    CHECK([[NSCharacterSet decimalDigitCharacterSet] characterIsMember:'7'] && ![[NSCharacterSet letterCharacterSet] characterIsMember:'7'], "NSCharacterSet");
    NSLog(@"NSLog works: %@ %d", @"yes", 1);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        testStrings();
        testNumbersAndCollections();
        testKVCAndArchiving();
        testFilesAndBundle();
        testCoreFoundation();
        testConcurrency();
        testMisc();
        printf("%d checks, %d failures\n", checks, failures);
        if (failures == 0) printf("ALL CHECKS PASSED\n");
    }
    return failures ? 1 : 0;
}
