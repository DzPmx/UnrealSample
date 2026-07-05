# Project Engineering Rules

## Mandatory Post-Implementation Review

After completing any implementation, perform a separate review pass before
declaring the work complete.

The review must:

1. Re-read the user's current request and the request checklist.
2. Review the complete diff produced for the request, not only the last edit.
3. Trace every changed line to an explicit requirement.
4. Check the resulting control flow, data flow, lifecycle, ownership, and edge
   cases for semantic regressions or contradictions with previously satisfied
   requirements.
5. Re-check all applicable project rules, including pointer and lambda capture
   rules.
6. Run the permitted verification steps appropriate to the change. Do not run
   compilation unless the user explicitly requested compilation in the current
   conversation.
7. Fix issues found by the review, then repeat the review on the updated diff.
8. Report each requested outcome as satisfied, partially satisfied, or not
   verified, including any remaining limitations.

Implementation and review are distinct passes. Do not treat reasoning performed
while writing the implementation as the required post-implementation review.
Do not declare completion until the review has been performed and reported.
