import Foundation

/// Recognises Epic's authorization-code page and pulls the code out of it.
///
/// Foundation only (no WebKit), so it can be compiled and unit-tested anywhere.
///
/// What Legendary's own manual login flow expects (Legendary 0.21.1):
///  * The user opens `EPCAPI.get_auth_url()` (legendary/api/egs.py:62-65), i.e.
///    `https://www.epicgames.com/id/login?redirectUrl=<https://www.epicgames.com/id/api/redirect
///    ?clientId=...&responseType=code>`; `https://legendary.gl/epiclogin` is a 302 to the same URL.
///  * After login the browser lands on `https://www.epicgames.com/id/api/redirect?clientId=...
///    &responseType=code`, whose body is a JSON object; Legendary tells the user to copy the
///    `authorizationCode` value out of it (README.md:145, cli.py:180-185).
///  * `LegendaryCore.auth_code(code)` (core.py:157) exchanges it.
public enum AuthCodeExtractor {
    /// Host and path of the page that carries the code (legendary/api/egs.py:64).
    public static let redirectHost = "www.epicgames.com"
    public static let redirectPath = "/id/api/redirect"
    /// JSON field holding the code (cli.py:184, README.md:145).
    public static let codeField = "authorizationCode"

    /// True when `url` is the code-carrying redirect page: https, Epic's host, that exact path.
    /// The query is not inspected: its `clientId` is whatever `login_url()` embedded.
    public static func isRedirectPage(_ url: URL?) -> Bool {
        guard let url, let comps = URLComponents(url: url, resolvingAgainstBaseURL: false) else {
            return false
        }
        return comps.scheme?.lowercased() == "https"
            && comps.host?.lowercased() == redirectHost
            && comps.path == redirectPath
    }

    public enum Result: Equatable {
        /// A non-empty authorization code.
        case code(String)
        /// A JSON object without a usable `authorizationCode` (absent, null or empty).
        case noCode
        /// The page text is not a JSON object.
        case notJSON
    }

    /// Parses the page's text content. Epic answers with a JSON object; WebKit shows
    /// a JSON document as plain text, so `document.body.innerText` yields it verbatim.
    public static func parse(pageText: String) -> Result {
        guard let data = pageText.trimmingCharacters(in: .whitespacesAndNewlines).data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return .notJSON }
        guard let code = object[codeField] as? String else { return .noCode }
        let trimmed = code.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? .noCode : .code(trimmed)
    }
}
