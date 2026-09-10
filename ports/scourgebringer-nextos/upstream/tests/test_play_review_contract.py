#!/usr/bin/env python3
"""Keep the late-game Play Core review bridge narrow and lifecycle-complete."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "jni_shim.c").read_text(encoding="utf-8")

FACTORY = "com.google.android.play.core.review.ReviewManagerFactory"
MANAGER = "com.google.android.play.core.review.ReviewManager"
TASK = "com.google.android.play.core.tasks.Task"

for class_name in (FACTORY, MANAGER, TASK):
    assert class_name in SOURCE, f"missing Play Core peer: {class_name}"

assert "is_play_review_factory(cls)" in SOURCE
assert SOURCE.count("is_play_review_factory(cls)") == 2, (
    "both CallStaticObjectMethod V/A paths must handle the factory"
)
assert 'strcmp(method, "create") == 0' in SOURCE
assert 'strcmp(method, "requestReviewFlow") == 0' in SOURCE
assert "is_play_review_manager(obj)" in SOURCE
assert "return play_review_task_object();" in SOURCE

# Do not regress to a global create() fallback. The exact-class predicate must
# remain on the same branch in both JNI static-call variants.
for function in ("CallStaticObjectMethodV", "CallStaticObjectMethodA"):
    start = SOURCE.index(f"static void *{function}")
    body = SOURCE[start : SOURCE.index("\n}", start) + 2]
    create = body.index('strcmp(method, "create") == 0')
    guard = body.index("is_play_review_factory(cls)", create)
    result = body.index("return play_review_manager_object();", guard)
    assert create < guard < result

print("play review JNI contract: OK")
