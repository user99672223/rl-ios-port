import Foundation

var failures = 0
func check(_ cond: Bool, _ what: String) {
    if cond { print("ok   \(what)") } else { print("FAIL \(what)"); failures += 1 }
}

// isRedirectPage
let good = URL(string: "https://www.epicgames.com/id/api/redirect?clientId=34a02cf8f4414e29b15921876da36f9a&responseType=code")
check(AuthCodeExtractor.isRedirectPage(good), "redirect page recognised")
check(AuthCodeExtractor.isRedirectPage(URL(string: "https://WWW.EPICGAMES.COM/id/api/redirect")), "host case-insensitive")
check(!AuthCodeExtractor.isRedirectPage(URL(string: "https://www.epicgames.com/id/login?redirectUrl=x")), "login page is not the redirect page")
check(!AuthCodeExtractor.isRedirectPage(URL(string: "http://www.epicgames.com/id/api/redirect")), "http rejected")
check(!AuthCodeExtractor.isRedirectPage(URL(string: "https://evil.example/id/api/redirect")), "other host rejected")
check(!AuthCodeExtractor.isRedirectPage(URL(string: "https://www.epicgames.com.evil.example/id/api/redirect")), "lookalike host rejected")
check(!AuthCodeExtractor.isRedirectPage(URL(string: "https://www.epicgames.com/id/api/redirectx")), "longer path rejected")
check(!AuthCodeExtractor.isRedirectPage(nil), "nil url")

// parse
let json = #"{"redirectUrl":"https://www.epicgames.com/id/api/redirect?x","authorizationCode":"abc123def456","exchangeCode":null,"sid":null,"ssoV2Enabled":true}"#
check(AuthCodeExtractor.parse(pageText: json) == .code("abc123def456"), "code extracted from Epic-shaped JSON")
check(AuthCodeExtractor.parse(pageText: "\n  " + json + "\n") == .code("abc123def456"), "surrounding whitespace")
check(AuthCodeExtractor.parse(pageText: #"{"authorizationCode":null}"#) == .noCode, "null code")
check(AuthCodeExtractor.parse(pageText: #"{"authorizationCode":"  "}"#) == .noCode, "blank code")
check(AuthCodeExtractor.parse(pageText: #"{"authorizationCode":123}"#) == .noCode, "non-string code")
check(AuthCodeExtractor.parse(pageText: #"{"errorCode":"errors.com.epicgames.x"}"#) == .noCode, "error object")
check(AuthCodeExtractor.parse(pageText: "<html>Sign in</html>") == .notJSON, "html is not JSON")
check(AuthCodeExtractor.parse(pageText: "[1,2]") == .notJSON, "array is not an object")
check(AuthCodeExtractor.parse(pageText: "") == .notJSON, "empty")

print(failures == 0 ? "ALL PASSED" : "\(failures) FAILED")
exit(failures == 0 ? 0 : 1)
