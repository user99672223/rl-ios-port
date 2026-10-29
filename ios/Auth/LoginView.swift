import SwiftUI
import WebKit

/// Why the sign-in page could not produce an authorization code.
public enum LoginFailure: Error, Equatable {
    /// Epic's redirect page loaded but its JSON had no `authorizationCode`.
    case noCodeInResponse
    /// The redirect page's body was not the JSON object Legendary documents.
    case unexpectedResponse
    /// WebKit reported a load failure (message is `NSError.localizedDescription`, no URL).
    case loadFailed(String)
}

/// First-login screen: shows Epic's sign-in page in a WKWebView, waits for the redirect page that
/// carries the authorization code, and hands the code to `onCode` exactly once.
///
/// Typical use (the app gets `loginURL` from `bridge.login_url()`, and passes the code to
/// `bridge.complete_login(code)` on a background thread):
///
///     LoginView(loginURL: url,
///               onCode: { code in /* complete_login(code) */ },
///               onFailure: { failure in /* show error */ })
///
/// Flow matches Legendary 0.21.1's own manual login (see `AuthCodeExtractor`): the page at
/// `https://www.epicgames.com/id/api/redirect` answers with JSON whose `authorizationCode` is the
/// value `legendary auth` asks the user to paste (cli.py:180-185).
///
/// Privacy: the code is never logged, printed or stored here. The web view uses a
/// non-persistent data store, so cookies and the Epic web session are discarded with the view
/// (Legendary likewise tears its login window down, utils/webview_login.py:64-66, 96-104).
///
/// Apple APIs used (all confirmed present in Apple's documentation data):
///  - UIViewRepresentable            developer.apple.com/documentation/swiftui/uiviewrepresentable
///  - WKWebsiteDataStore.nonPersistent()   .../webkit/wkwebsitedatastore/nonpersistent()
///  - WKWebViewConfiguration.websiteDataStore   .../webkit/wkwebviewconfiguration/websitedatastore
///  - WKNavigationDelegate.webView(_:didFinish:)   .../webkit/wknavigationdelegate/webview(_:didfinish:)
///  - ...webView(_:didFail:withError:) and ...didFailProvisionalNavigation:withError:
///  - WKWebView.evaluateJavaScript(_:completionHandler:)   .../webkit/wkwebview/evaluatejavascript(_:completionhandler:)
///  - WKWebView.stopLoading()        .../webkit/wkwebview/stoploading()
///  - WKUIDelegate.webView(_:createWebViewWith:for:windowFeatures:)   .../webkit/wkuidelegate/...
public struct LoginView: UIViewRepresentable {
    public let loginURL: URL
    public let onCode: (String) -> Void
    public let onFailure: (LoginFailure) -> Void

    public init(loginURL: URL,
                onCode: @escaping (String) -> Void,
                onFailure: @escaping (LoginFailure) -> Void = { _ in }) {
        self.loginURL = loginURL
        self.onCode = onCode
        self.onFailure = onFailure
    }

    public func makeCoordinator() -> Coordinator {
        Coordinator(onCode: onCode, onFailure: onFailure)
    }

    public func makeUIView(context: Context) -> WKWebView {
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = .nonPersistent()
        let webView = WKWebView(frame: .zero, configuration: configuration)
        webView.navigationDelegate = context.coordinator
        webView.uiDelegate = context.coordinator
        webView.load(URLRequest(url: loginURL))
        return webView
    }

    public func updateUIView(_ uiView: WKWebView, context: Context) {}

    public final class Coordinator: NSObject, WKNavigationDelegate, WKUIDelegate {
        private let onCode: (String) -> Void
        private let onFailure: (LoginFailure) -> Void
        private var finished = false

        init(onCode: @escaping (String) -> Void, onFailure: @escaping (LoginFailure) -> Void) {
            self.onCode = onCode
            self.onFailure = onFailure
        }

        // The redirect page has loaded: read its body text and look for the code.
        public func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
            guard !finished, AuthCodeExtractor.isRedirectPage(webView.url) else { return }
            webView.evaluateJavaScript("document.body ? document.body.innerText : ''") { [weak self, weak webView] value, _ in
                guard let self, !self.finished else { return }
                guard let text = value as? String else {
                    self.fail(.unexpectedResponse)
                    return
                }
                switch AuthCodeExtractor.parse(pageText: text) {
                case .code(let code):
                    self.finished = true
                    webView?.stopLoading()
                    self.onCode(code)
                case .noCode:
                    self.fail(.noCodeInResponse)
                case .notJSON:
                    self.fail(.unexpectedResponse)
                }
            }
        }

        public func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
            loadFailed(error)
        }

        public func webView(_ webView: WKWebView,
                            didFailProvisionalNavigation navigation: WKNavigation!,
                            withError error: Error) {
            loadFailed(error)
        }

        // Sign-in options (external identity providers, captcha) may call window.open /
        // target=_blank. Load those in the same view; do not spawn a second web view.
        public func webView(_ webView: WKWebView,
                            createWebViewWith configuration: WKWebViewConfiguration,
                            for navigationAction: WKNavigationAction,
                            windowFeatures: WKWindowFeatures) -> WKWebView? {
            if navigationAction.targetFrame == nil {
                webView.load(navigationAction.request)
            }
            return nil
        }

        // A navigation superseded by another one (login redirects, our own stopLoading) reports
        // NSURLErrorCancelled; that is not a failure.
        private func loadFailed(_ error: Error) {
            let nsError = error as NSError
            if nsError.domain == NSURLErrorDomain && nsError.code == NSURLErrorCancelled { return }
            fail(.loadFailed(error.localizedDescription))
        }

        private func fail(_ failure: LoginFailure) {
            guard !finished else { return }
            onFailure(failure)
        }
    }
}
