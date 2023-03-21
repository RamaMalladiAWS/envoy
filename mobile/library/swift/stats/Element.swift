import Foundation

private let kPattern = "^[A-Za-z_]+$"

/// Element represents one dot-delimited component of a time series name.
/// Element values must conform to the regex /^[A-Za-z_]+$/.
@objcMembers
public final class Element: NSObject, ExpressibleByStringLiteral {
  internal let value: String

  public init(stringLiteral value: String) {
    precondition(
      value.matchesStatsElementPattern,
      "Element values must conform to the regex '\(kPattern)'."
    )
    self.value = value
  }

  public override func isEqual(_ object: Any?) -> Bool {
    return (object as? Element)?.value == self.value
  }
}

extension String {
  var matchesStatsElementPattern: Bool {
    if #available(iOS 16.0, macOS 13.0, *) {
      return self.contains(/^[A-Za-z_]+$/)
    }

    // `std` is the name of the C++ stdlib when importing it into Swift.
    // So effectively this checks if we're compiling with `-enable-experimental-cxx-interop`.
#if canImport(std)
    return (self as NSString).range(of: kPattern, options: regularExpression).location != NSNotFound
#else
    return self.range(of: kPattern, options: .regularExpression) != nil
#endif
  }
}
