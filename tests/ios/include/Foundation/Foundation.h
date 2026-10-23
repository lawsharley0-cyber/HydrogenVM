// Minimal Foundation declarations for HydrogenVM's test apps (32-bit iOS ABI).
// Only what the tests use; signatures match the iOS SDK.
#pragma once
#include <objc/NSObject.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#define NS_REQUIRES_NIL_TERMINATION __attribute__((sentinel(0, 1)))
#define NS_FORMAT_FUNCTION(F, A) __attribute__((format(__NSString__, F, A)))
#define NSNotFound ((NSInteger)0x7fffffff)
typedef double NSTimeInterval;
typedef NSUInteger NSStringEncoding;
typedef struct _NSRange { NSUInteger location, length; } NSRange;
static inline NSRange NSMakeRange(NSUInteger loc, NSUInteger len) { NSRange r = {loc, len}; return r; }
static inline NSUInteger NSMaxRange(NSRange r) { return r.location + r.length; }
typedef NSInteger NSComparisonResult;
enum { NSOrderedAscending = -1, NSOrderedSame, NSOrderedDescending };
typedef NSComparisonResult (^NSComparator)(id obj1, id obj2);
enum { NSASCIIStringEncoding = 1, NSUTF8StringEncoding = 4, NSISOLatin1StringEncoding = 5, NSUnicodeStringEncoding = 10 };
enum { NSCaseInsensitiveSearch = 1, NSLiteralSearch = 2, NSBackwardsSearch = 4, NSAnchoredSearch = 8, NSNumericSearch = 64 };
enum { NSDocumentDirectory = 9, NSLibraryDirectory = 5, NSCachesDirectory = 13 };
enum { NSUserDomainMask = 1 };
enum { NSKeyValueObservingOptionNew = 1, NSKeyValueObservingOptionOld = 2 };
typedef NSUInteger NSEnumerationOptions;
enum { NSEnumerationConcurrent = 1, NSEnumerationReverse = 2 };
enum { NSJSONReadingMutableContainers = 1, NSJSONReadingMutableLeaves = 2, NSJSONReadingAllowFragments = 4 };
enum { NSJSONWritingPrettyPrinted = 1 };
enum { NSPropertyListImmutable = 0, NSPropertyListMutableContainers = 1 };
enum { NSPropertyListXMLFormat_v1_0 = 100, NSPropertyListBinaryFormat_v1_0 = 200 };

@class NSArray, NSDictionary, NSData, NSError, NSURL, NSDate, NSSet, NSCharacterSet, NSLocale, NSMutableArray, NSMutableDictionary;

typedef struct {
    unsigned long state;
    id __unsafe_unretained *itemsPtr;
    unsigned long *mutationsPtr;
    unsigned long extra[5];
} NSFastEnumerationState;
@protocol NSFastEnumeration
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state objects:(id __unsafe_unretained [])buffer count:(NSUInteger)len;
@end
@protocol NSCoding
- (void)encodeWithCoder:(id)aCoder;
- (id)initWithCoder:(id)aDecoder;
@end

@interface NSObject (NSFoundationAdditions)
+ (NSString *)description;
- (NSString *)debugDescription;
- (id)valueForKey:(NSString *)key;
- (void)setValue:(id)value forKey:(NSString *)key;
- (id)valueForKeyPath:(NSString *)keyPath;
- (void)setValue:(id)value forKeyPath:(NSString *)keyPath;
- (void)setValuesForKeysWithDictionary:(NSDictionary *)keyedValues;
- (void)performSelector:(SEL)aSelector withObject:(id)anArgument afterDelay:(NSTimeInterval)delay;
- (void)performSelectorOnMainThread:(SEL)aSelector withObject:(id)arg waitUntilDone:(BOOL)wait;
- (void)performSelectorInBackground:(SEL)aSelector withObject:(id)arg;
+ (void)cancelPreviousPerformRequestsWithTarget:(id)aTarget;
+ (void)cancelPreviousPerformRequestsWithTarget:(id)aTarget selector:(SEL)aSelector object:(id)anArgument;
- (void)addObserver:(NSObject *)observer forKeyPath:(NSString *)keyPath options:(NSUInteger)options context:(void *)context;
- (void)removeObserver:(NSObject *)observer forKeyPath:(NSString *)keyPath;
@end

@interface NSString : NSObject <NSCopying, NSMutableCopying>
+ (instancetype)string;
+ (instancetype)stringWithString:(NSString *)s;
+ (instancetype)stringWithUTF8String:(const char *)s;
+ (instancetype)stringWithFormat:(NSString *)format, ... NS_FORMAT_FUNCTION(1, 2);
+ (instancetype)stringWithCharacters:(const unsigned short *)c length:(NSUInteger)n;
+ (instancetype)stringWithContentsOfFile:(NSString *)path encoding:(NSStringEncoding)enc error:(NSError **)error;
+ (instancetype)stringWithContentsOfURL:(NSURL *)url encoding:(NSStringEncoding)enc error:(NSError **)error;
+ (instancetype)stringWithContentsOfURL:(NSURL *)url usedEncoding:(NSStringEncoding *)enc error:(NSError **)error;
+ (instancetype)stringWithContentsOfURL:(NSURL *)url;
+ (NSString *)pathWithComponents:(NSArray *)components;
- (instancetype)initWithFormat:(NSString *)format, ... NS_FORMAT_FUNCTION(1, 2);
- (instancetype)initWithContentsOfURL:(NSURL *)url encoding:(NSStringEncoding)enc error:(NSError **)error;
- (instancetype)initWithContentsOfURL:(NSURL *)url usedEncoding:(NSStringEncoding *)enc error:(NSError **)error;
- (instancetype)initWithContentsOfURL:(NSURL *)url;
- (instancetype)initWithFormat:(NSString *)format arguments:(va_list)args;
- (instancetype)initWithUTF8String:(const char *)s;
- (instancetype)initWithData:(NSData *)data encoding:(NSStringEncoding)enc;
- (instancetype)initWithBytes:(const void *)bytes length:(NSUInteger)len encoding:(NSStringEncoding)enc;
- (NSUInteger)length;
- (unsigned short)characterAtIndex:(NSUInteger)i;
- (const char *)UTF8String;
- (const char *)cStringUsingEncoding:(NSStringEncoding)enc;
- (NSData *)dataUsingEncoding:(NSStringEncoding)enc;
- (BOOL)isEqualToString:(NSString *)s;
- (NSComparisonResult)compare:(NSString *)s;
- (NSComparisonResult)compare:(NSString *)s options:(NSUInteger)mask;
- (NSComparisonResult)caseInsensitiveCompare:(NSString *)s;
- (BOOL)hasPrefix:(NSString *)s;
- (BOOL)hasSuffix:(NSString *)s;
- (NSRange)rangeOfString:(NSString *)s;
- (NSRange)rangeOfString:(NSString *)s options:(NSUInteger)mask;
- (NSRange)rangeOfCharacterFromSet:(NSCharacterSet *)set;
- (NSString *)substringFromIndex:(NSUInteger)i;
- (NSString *)substringToIndex:(NSUInteger)i;
- (NSString *)substringWithRange:(NSRange)r;
- (NSString *)stringByAppendingString:(NSString *)s;
- (NSString *)stringByAppendingFormat:(NSString *)format, ... NS_FORMAT_FUNCTION(1, 2);
- (NSString *)stringByReplacingOccurrencesOfString:(NSString *)target withString:(NSString *)replacement;
- (NSString *)stringByTrimmingCharactersInSet:(NSCharacterSet *)set;
- (NSString *)stringByPaddingToLength:(NSUInteger)len withString:(NSString *)pad startingAtIndex:(NSUInteger)i;
- (NSArray *)componentsSeparatedByString:(NSString *)s;
- (NSArray *)componentsSeparatedByCharactersInSet:(NSCharacterSet *)set;
- (NSString *)uppercaseString;
- (NSString *)lowercaseString;
- (NSString *)capitalizedString;
- (int)intValue;
- (NSInteger)integerValue;
- (long long)longLongValue;
- (float)floatValue;
- (double)doubleValue;
- (BOOL)boolValue;
- (NSString *)lastPathComponent;
- (NSString *)pathExtension;
- (NSString *)stringByDeletingLastPathComponent;
- (NSString *)stringByDeletingPathExtension;
- (NSString *)stringByAppendingPathComponent:(NSString *)s;
- (NSString *)stringByAppendingPathExtension:(NSString *)s;
- (NSArray *)pathComponents;
- (BOOL)writeToFile:(NSString *)path atomically:(BOOL)a encoding:(NSStringEncoding)enc error:(NSError **)error;
- (NSString *)stringByAddingPercentEscapesUsingEncoding:(NSStringEncoding)enc;
- (NSString *)stringByReplacingPercentEscapesUsingEncoding:(NSStringEncoding)enc;
@end
@interface NSMutableString : NSString
+ (instancetype)stringWithCapacity:(NSUInteger)n;
- (void)appendString:(NSString *)s;
- (void)appendFormat:(NSString *)format, ... NS_FORMAT_FUNCTION(1, 2);
- (void)setString:(NSString *)s;
- (void)insertString:(NSString *)s atIndex:(NSUInteger)i;
- (void)deleteCharactersInRange:(NSRange)r;
- (void)replaceCharactersInRange:(NSRange)r withString:(NSString *)s;
- (NSUInteger)replaceOccurrencesOfString:(NSString *)t withString:(NSString *)r options:(NSUInteger)o range:(NSRange)range;
@end

@interface NSCharacterSet : NSObject <NSCopying, NSMutableCopying>
+ (NSCharacterSet *)whitespaceCharacterSet;
+ (NSCharacterSet *)whitespaceAndNewlineCharacterSet;
+ (NSCharacterSet *)decimalDigitCharacterSet;
+ (NSCharacterSet *)letterCharacterSet;
+ (NSCharacterSet *)punctuationCharacterSet;
+ (NSCharacterSet *)characterSetWithCharactersInString:(NSString *)s;
- (BOOL)characterIsMember:(unsigned short)c;
- (NSCharacterSet *)invertedSet;
@end

@interface NSValue : NSObject <NSCopying>
+ (NSValue *)valueWithRange:(NSRange)range;
+ (NSValue *)valueWithPointer:(const void *)p;
+ (NSValue *)valueWithBytes:(const void *)value objCType:(const char *)type;
- (NSRange)rangeValue;
- (void *)pointerValue;
- (void)getValue:(void *)value;
@end
@interface NSNumber : NSValue
+ (NSNumber *)numberWithInt:(int)v;
+ (NSNumber *)numberWithInteger:(NSInteger)v;
+ (NSNumber *)numberWithUnsignedInt:(unsigned int)v;
+ (NSNumber *)numberWithLongLong:(long long)v;
+ (NSNumber *)numberWithFloat:(float)v;
+ (NSNumber *)numberWithDouble:(double)v;
+ (NSNumber *)numberWithBool:(BOOL)v;
- (instancetype)initWithInt:(int)v;
- (int)intValue;
- (NSInteger)integerValue;
- (long long)longLongValue;
- (float)floatValue;
- (double)doubleValue;
- (BOOL)boolValue;
- (NSString *)stringValue;
- (NSComparisonResult)compare:(NSNumber *)n;
- (BOOL)isEqualToNumber:(NSNumber *)n;
- (const char *)objCType;
@end

@interface NSArray : NSObject <NSCopying, NSMutableCopying, NSFastEnumeration>
+ (instancetype)array;
+ (instancetype)arrayWithObject:(id)o;
+ (instancetype)arrayWithObjects:(id)first, ... NS_REQUIRES_NIL_TERMINATION;
+ (instancetype)arrayWithObjects:(const id [])objects count:(NSUInteger)n;
+ (instancetype)arrayWithArray:(NSArray *)a;
+ (instancetype)arrayWithContentsOfFile:(NSString *)path;
- (instancetype)initWithObjects:(id)first, ... NS_REQUIRES_NIL_TERMINATION;
- (NSUInteger)count;
- (id)objectAtIndex:(NSUInteger)i;
- (id)objectAtIndexedSubscript:(NSUInteger)i;
- (id)firstObject;
- (id)lastObject;
- (BOOL)containsObject:(id)o;
- (NSUInteger)indexOfObject:(id)o;
- (NSArray *)arrayByAddingObject:(id)o;
- (NSArray *)arrayByAddingObjectsFromArray:(NSArray *)a;
- (NSArray *)subarrayWithRange:(NSRange)r;
- (NSString *)componentsJoinedByString:(NSString *)s;
- (NSArray *)sortedArrayUsingSelector:(SEL)s;
- (NSArray *)sortedArrayUsingComparator:(NSComparator)cmp;
- (NSArray *)sortedArrayUsingDescriptors:(NSArray *)sortDescriptors;
- (NSArray *)filteredArrayUsingPredicate:(id)p;
- (void)enumerateObjectsUsingBlock:(void (^)(id obj, NSUInteger idx, BOOL *stop))block;
- (void)makeObjectsPerformSelector:(SEL)s;
- (BOOL)writeToFile:(NSString *)path atomically:(BOOL)a;
- (BOOL)isEqualToArray:(NSArray *)a;
- (NSArray *)reverseObjectEnumerator;
@end
@interface NSMutableArray : NSArray
+ (instancetype)arrayWithCapacity:(NSUInteger)n;
- (void)addObject:(id)o;
- (void)addObjectsFromArray:(NSArray *)a;
- (void)insertObject:(id)o atIndex:(NSUInteger)i;
- (void)removeObjectAtIndex:(NSUInteger)i;
- (void)removeObject:(id)o;
- (void)removeLastObject;
- (void)removeAllObjects;
- (void)replaceObjectAtIndex:(NSUInteger)i withObject:(id)o;
- (void)setObject:(id)o atIndexedSubscript:(NSUInteger)i;
- (void)exchangeObjectAtIndex:(NSUInteger)a withObjectAtIndex:(NSUInteger)b;
- (void)sortUsingSelector:(SEL)s;
- (void)sortUsingComparator:(NSComparator)cmp;
@end

@interface NSDictionary : NSObject <NSCopying, NSMutableCopying, NSFastEnumeration>
+ (instancetype)dictionary;
+ (instancetype)dictionaryWithObject:(id)o forKey:(id)k;
+ (instancetype)dictionaryWithObjectsAndKeys:(id)first, ... NS_REQUIRES_NIL_TERMINATION;
+ (instancetype)dictionaryWithObjects:(const id [])objects forKeys:(const id [])keys count:(NSUInteger)n;
+ (instancetype)dictionaryWithDictionary:(NSDictionary *)d;
+ (instancetype)dictionaryWithContentsOfFile:(NSString *)path;
- (NSUInteger)count;
- (id)objectForKey:(id)k;
- (id)objectForKeyedSubscript:(id)k;
- (NSArray *)allKeys;
- (NSArray *)allValues;
- (NSArray *)keysSortedByValueUsingSelector:(SEL)s;
- (void)enumerateKeysAndObjectsUsingBlock:(void (^)(id key, id obj, BOOL *stop))block;
- (BOOL)writeToFile:(NSString *)path atomically:(BOOL)a;
- (BOOL)isEqualToDictionary:(NSDictionary *)d;
@end
@interface NSMutableDictionary : NSDictionary
+ (instancetype)dictionaryWithCapacity:(NSUInteger)n;
- (void)setObject:(id)o forKey:(id)k;
- (void)setObject:(id)o forKeyedSubscript:(id)k;
- (void)removeObjectForKey:(id)k;
- (void)removeAllObjects;
- (void)addEntriesFromDictionary:(NSDictionary *)d;
@end

@interface NSSet : NSObject <NSCopying, NSMutableCopying, NSFastEnumeration>
+ (instancetype)set;
+ (instancetype)setWithObjects:(id)first, ... NS_REQUIRES_NIL_TERMINATION;
+ (instancetype)setWithArray:(NSArray *)a;
- (NSUInteger)count;
- (BOOL)containsObject:(id)o;
- (id)anyObject;
- (NSArray *)allObjects;
- (id)member:(id)o;
@end
@interface NSMutableSet : NSSet
- (void)addObject:(id)o;
- (void)removeObject:(id)o;
@end

@interface NSData : NSObject <NSCopying, NSMutableCopying>
+ (instancetype)data;
+ (instancetype)dataWithBytes:(const void *)bytes length:(NSUInteger)n;
+ (instancetype)dataWithContentsOfFile:(NSString *)path;
- (NSUInteger)length;
- (const void *)bytes;
- (BOOL)writeToFile:(NSString *)path atomically:(BOOL)a;
- (BOOL)isEqualToData:(NSData *)d;
- (NSData *)subdataWithRange:(NSRange)r;
@end
@interface NSMutableData : NSData
+ (instancetype)dataWithCapacity:(NSUInteger)n;
+ (instancetype)dataWithLength:(NSUInteger)n;
- (void *)mutableBytes;
- (void)appendBytes:(const void *)b length:(NSUInteger)n;
- (void)appendData:(NSData *)d;
- (void)setLength:(NSUInteger)n;
@end

@interface NSDate : NSObject <NSCopying>
+ (instancetype)date;
+ (instancetype)dateWithTimeIntervalSinceNow:(NSTimeInterval)s;
+ (instancetype)dateWithTimeIntervalSince1970:(NSTimeInterval)s;
+ (instancetype)distantFuture;
+ (NSTimeInterval)timeIntervalSinceReferenceDate;
- (NSTimeInterval)timeIntervalSinceNow;
- (NSTimeInterval)timeIntervalSince1970;
- (NSTimeInterval)timeIntervalSinceDate:(NSDate *)d;
- (NSDate *)dateByAddingTimeInterval:(NSTimeInterval)s;
- (NSComparisonResult)compare:(NSDate *)d;
@end

@interface NSNull : NSObject
+ (NSNull *)null;
@end

@interface NSError : NSObject
+ (instancetype)errorWithDomain:(NSString *)domain code:(NSInteger)code userInfo:(NSDictionary *)info;
- (NSString *)domain;
- (NSInteger)code;
- (NSDictionary *)userInfo;
- (NSString *)localizedDescription;
@end
extern NSString *const NSCocoaErrorDomain;
extern NSString *const NSLocalizedDescriptionKey;

@interface NSException : NSObject
+ (NSException *)exceptionWithName:(NSString *)name reason:(NSString *)reason userInfo:(NSDictionary *)info;
+ (void)raise:(NSString *)name format:(NSString *)format, ... NS_FORMAT_FUNCTION(2, 3);
- (NSString *)name;
- (NSString *)reason;
- (void)raise;
@end
extern NSString *const NSInvalidArgumentException;
extern NSString *const NSRangeException;

@interface NSAutoreleasePool : NSObject
- (void)drain;
@end

@interface NSRunLoop : NSObject
+ (NSRunLoop *)currentRunLoop;
+ (NSRunLoop *)mainRunLoop;
- (void)run;
- (void)runUntilDate:(NSDate *)d;
- (BOOL)runMode:(NSString *)mode beforeDate:(NSDate *)d;
- (void)addTimer:(id)timer forMode:(NSString *)mode;
@end
extern NSString *const NSDefaultRunLoopMode;
extern NSString *const NSRunLoopCommonModes;

@interface NSTimer : NSObject
+ (NSTimer *)scheduledTimerWithTimeInterval:(NSTimeInterval)ti target:(id)t selector:(SEL)s userInfo:(id)ui repeats:(BOOL)r;
+ (NSTimer *)timerWithTimeInterval:(NSTimeInterval)ti target:(id)t selector:(SEL)s userInfo:(id)ui repeats:(BOOL)r;
- (void)invalidate;
- (BOOL)isValid;
- (id)userInfo;
- (void)fire;
@end

@interface NSThread : NSObject
+ (NSThread *)currentThread;
+ (NSThread *)mainThread;
+ (BOOL)isMainThread;
+ (void)sleepForTimeInterval:(NSTimeInterval)ti;
+ (void)detachNewThreadSelector:(SEL)s toTarget:(id)t withObject:(id)arg;
- (BOOL)isMainThread;
- (NSMutableDictionary *)threadDictionary;
@end

@interface NSLock : NSObject
- (void)lock;
- (void)unlock;
- (BOOL)tryLock;
@end
@interface NSCondition : NSObject
- (void)lock;
- (void)unlock;
- (void)wait;
- (void)signal;
- (void)broadcast;
@end

@interface NSBundle : NSObject
+ (NSBundle *)mainBundle;
- (NSString *)bundlePath;
- (NSString *)resourcePath;
- (NSString *)bundleIdentifier;
- (NSDictionary *)infoDictionary;
- (id)objectForInfoDictionaryKey:(NSString *)key;
- (NSString *)pathForResource:(NSString *)name ofType:(NSString *)ext;
- (NSURL *)URLForResource:(NSString *)name withExtension:(NSString *)ext;
- (NSString *)localizedStringForKey:(NSString *)key value:(NSString *)value table:(NSString *)table;
@end
#define NSLocalizedString(key, comment) [[NSBundle mainBundle] localizedStringForKey:(key) value:@"" table:nil]

@interface NSURL : NSObject <NSCopying>
+ (instancetype)URLWithString:(NSString *)s;
+ (instancetype)fileURLWithPath:(NSString *)p;
- (NSString *)absoluteString;
- (NSString *)path;
- (NSString *)scheme;
- (NSString *)host;
- (NSNumber *)port;
- (NSString *)query;
- (NSString *)fragment;
- (NSString *)lastPathComponent;
- (BOOL)isFileURL;
- (NSURL *)URLByAppendingPathComponent:(NSString *)c;
@end

@interface NSFileManager : NSObject
+ (NSFileManager *)defaultManager;
- (BOOL)fileExistsAtPath:(NSString *)path;
- (BOOL)fileExistsAtPath:(NSString *)path isDirectory:(BOOL *)isDir;
- (BOOL)createDirectoryAtPath:(NSString *)path withIntermediateDirectories:(BOOL)i attributes:(NSDictionary *)a error:(NSError **)e;
- (BOOL)removeItemAtPath:(NSString *)path error:(NSError **)e;
- (BOOL)copyItemAtPath:(NSString *)src toPath:(NSString *)dst error:(NSError **)e;
- (NSArray *)contentsOfDirectoryAtPath:(NSString *)path error:(NSError **)e;
- (NSDictionary *)attributesOfItemAtPath:(NSString *)path error:(NSError **)e;
- (NSArray *)URLsForDirectory:(NSUInteger)dir inDomains:(NSUInteger)mask;
@end
extern NSString *const NSFileSize;
NSArray *NSSearchPathForDirectoriesInDomains(NSUInteger dir, NSUInteger mask, BOOL expand);
NSString *NSHomeDirectory(void);
NSString *NSTemporaryDirectory(void);

@interface NSUserDefaults : NSObject
+ (NSUserDefaults *)standardUserDefaults;
- (id)objectForKey:(NSString *)k;
- (void)setObject:(id)o forKey:(NSString *)k;
- (void)removeObjectForKey:(NSString *)k;
- (NSString *)stringForKey:(NSString *)k;
- (NSInteger)integerForKey:(NSString *)k;
- (BOOL)boolForKey:(NSString *)k;
- (float)floatForKey:(NSString *)k;
- (double)doubleForKey:(NSString *)k;
- (void)setInteger:(NSInteger)v forKey:(NSString *)k;
- (void)setBool:(BOOL)v forKey:(NSString *)k;
- (void)setFloat:(float)v forKey:(NSString *)k;
- (void)setDouble:(double)v forKey:(NSString *)k;
- (void)registerDefaults:(NSDictionary *)d;
- (BOOL)synchronize;
@end

@interface NSNotification : NSObject
- (NSString *)name;
- (id)object;
- (NSDictionary *)userInfo;
@end
@interface NSNotificationCenter : NSObject
+ (NSNotificationCenter *)defaultCenter;
- (void)addObserver:(id)o selector:(SEL)s name:(NSString *)n object:(id)obj;
- (id)addObserverForName:(NSString *)n object:(id)obj queue:(id)q usingBlock:(void (^)(NSNotification *note))block;
- (void)removeObserver:(id)o;
- (void)postNotificationName:(NSString *)n object:(id)obj;
- (void)postNotificationName:(NSString *)n object:(id)obj userInfo:(NSDictionary *)info;
@end

@interface NSOperation : NSObject
- (void)start;
- (void)main;
- (void)cancel;
- (BOOL)isCancelled;
- (BOOL)isFinished;
- (void)addDependency:(NSOperation *)op;
- (void)setCompletionBlock:(void (^)(void))block;
- (void)waitUntilFinished;
@end
@interface NSBlockOperation : NSOperation
+ (instancetype)blockOperationWithBlock:(void (^)(void))block;
@end
@interface NSOperationQueue : NSObject
+ (NSOperationQueue *)mainQueue;
+ (NSOperationQueue *)currentQueue;
- (void)addOperation:(NSOperation *)op;
- (void)addOperationWithBlock:(void (^)(void))block;
- (void)waitUntilAllOperationsAreFinished;
- (void)setMaxConcurrentOperationCount:(NSInteger)n;
- (NSUInteger)operationCount;
@end

@interface NSProcessInfo : NSObject
+ (NSProcessInfo *)processInfo;
- (NSString *)processName;
- (NSArray *)arguments;
- (NSDictionary *)environment;
- (NSString *)globallyUniqueString;
- (NSString *)operatingSystemVersionString;
- (NSTimeInterval)systemUptime;
@end

@interface NSIndexPath : NSObject <NSCopying>
+ (NSIndexPath *)indexPathForRow:(NSInteger)row inSection:(NSInteger)section;
- (NSInteger)row;
- (NSInteger)section;
@end
@interface NSIndexSet : NSObject <NSCopying, NSMutableCopying>
+ (instancetype)indexSetWithIndexesInRange:(NSRange)r;
- (NSUInteger)count;
- (BOOL)containsIndex:(NSUInteger)i;
- (NSUInteger)firstIndex;
- (NSUInteger)lastIndex;
@end
@interface NSMutableIndexSet : NSIndexSet
- (void)addIndex:(NSUInteger)i;
- (void)removeIndex:(NSUInteger)i;
@end

@interface NSUUID : NSObject <NSCopying>
+ (instancetype)UUID;
- (NSString *)UUIDString;
@end

@interface NSSortDescriptor : NSObject <NSCopying>
+ (instancetype)sortDescriptorWithKey:(NSString *)key ascending:(BOOL)ascending;
+ (instancetype)sortDescriptorWithKey:(NSString *)key ascending:(BOOL)ascending selector:(SEL)selector;
- (instancetype)initWithKey:(NSString *)key ascending:(BOOL)ascending;
- (instancetype)initWithKey:(NSString *)key ascending:(BOOL)ascending selector:(SEL)selector;
@property(readonly, copy) NSString *key;
@property(readonly) BOOL ascending;
@property(readonly) SEL selector;
- (NSComparisonResult)compareObject:(id)object1 toObject:(id)object2;
- (instancetype)reversedSortDescriptor;
@end

@interface NSJSONSerialization : NSObject
+ (id)JSONObjectWithData:(NSData *)data options:(NSUInteger)opt error:(NSError **)error;
+ (NSData *)dataWithJSONObject:(id)obj options:(NSUInteger)opt error:(NSError **)error;
+ (BOOL)isValidJSONObject:(id)obj;
@end
@interface NSPropertyListSerialization : NSObject
+ (NSData *)dataWithPropertyList:(id)plist format:(NSUInteger)format options:(NSUInteger)opt error:(NSError **)error;
+ (id)propertyListWithData:(NSData *)data options:(NSUInteger)opt format:(NSUInteger *)format error:(NSError **)error;
@end
@interface NSCoder : NSObject
- (void)encodeObject:(id)o forKey:(NSString *)k;
- (void)encodeInt:(int)v forKey:(NSString *)k;
- (void)encodeInteger:(NSInteger)v forKey:(NSString *)k;
- (void)encodeFloat:(float)v forKey:(NSString *)k;
- (void)encodeDouble:(double)v forKey:(NSString *)k;
- (void)encodeBool:(BOOL)v forKey:(NSString *)k;
- (id)decodeObjectForKey:(NSString *)k;
- (int)decodeIntForKey:(NSString *)k;
- (NSInteger)decodeIntegerForKey:(NSString *)k;
- (float)decodeFloatForKey:(NSString *)k;
- (double)decodeDoubleForKey:(NSString *)k;
- (BOOL)decodeBoolForKey:(NSString *)k;
- (BOOL)containsValueForKey:(NSString *)k;
@end
@interface NSKeyedArchiver : NSCoder
+ (NSData *)archivedDataWithRootObject:(id)root;
+ (BOOL)archiveRootObject:(id)root toFile:(NSString *)path;
@end
@interface NSKeyedUnarchiver : NSCoder
+ (id)unarchiveObjectWithData:(NSData *)data;
+ (id)unarchiveObjectWithFile:(NSString *)path;
@end

void NSLog(NSString *format, ...) NS_FORMAT_FUNCTION(1, 2);
NSString *NSStringFromClass(Class c);
Class NSClassFromString(NSString *s);
NSString *NSStringFromSelector(SEL s);
SEL NSSelectorFromString(NSString *s);
NSString *NSStringFromRange(NSRange r);
