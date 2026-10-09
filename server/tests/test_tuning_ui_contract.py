"""PID Tuning: edit-mode banner, badge vocabulary, action clustering, results."""

from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1] / "app"
TUNING_JS = APP_DIR / "static" / "js" / "tuning.js"
TUNING_HTML = APP_DIR / "templates" / "tuning.html"


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def _js_code(text: str) -> str:
    """Strip JS comments so a negative token assert cannot be satisfied — or
    defeated — by documentation. Product comments name the Number(x || 0) trap
    on purpose; the assert must bite on code that uses it."""
    import re
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def test_edit_banner_names_the_version_and_says_saving_creates_the_next():
    """Regression pin — already green before Task 5; the behaviour must not regress."""
    js = _read(TUNING_JS)
    body = _js_code(js.split("function updateEditBanner", 1)[1].split("function startEdit", 1)[0])
    assert "Editing" in body
    assert "saving creates" in body.lower() or "Saving will not overwrite" in body
    # The message must interpolate the live version into the Editing line.
    # editingSource.version appears twice in the function; pin both sites so
    # dropping either (leaving a dangling "v") cannot stay green on the other.
    assert "editingSource.version + ' — saving creates v'" in body
    assert "' and historical runs remain linked to v' + editingSource.version" in body


def test_badge_vocabulary_matches_what_the_api_can_express():
    """Three row badges the API can actually reach, plus the summary badge.
    DEACTIVATED is deliberately absent: TuningProfileOut has no `deactivated`
    field and no history to derive one, so the branch was unreachable and the
    frontend plan forbids adding it. Pin each label with its tone and the
    branch order — an ambient `word in js` loop stays green on a dead branch,
    a swapped return, or a tone change, which is exactly how the unreachable
    DEACTIVATED badge survived."""
    js = _read(TUNING_JS)
    body = js.split("function profileStatus", 1)[1].split("function updateTargetOptions", 1)[0]
    assert "deviceProfile(profile)) return { label: 'APPLIED', tone: 'success' }" in body
    assert "profile.active) return { label: 'PENDING DEVICE', tone: 'warning' }" in body
    assert "return { label: 'HISTORICAL', tone: 'neutral' }" in body
    assert "DEACTIVATED" not in body
    assert "profile.deactivated" not in body
    # Branch order is load-bearing: deviceProfile must win over profile.active,
    # or a live-but-not-active row would read PENDING DEVICE.
    assert body.index("deviceProfile(profile)") < body.index("profile.active")
    assert "NO ACTIVE PROFILE" in js


def test_delete_is_not_beside_apply():
    js = _read(TUNING_JS)
    # NB: renderProfiles' map callback has a nested `function `, which truncates
    # split("function ", 1)[0] at 73 chars. Capture the named helper instead.
    body = js.split("function profileActions", 1)[1].split("function ", 1)[0]
    assert 'data-profile-action="delete"' in body
    assert 'data-profile-action="activate"' in body
    assert 'data-profile-action="deactivate"' in body
    # Delete sits in its own cluster, separated from the primary actions.
    assert 'class="action-cluster"' in body
    assert 'class="btn ghost sm" data-profile-action="delete"' in body
    # Apply and Delete are never adjacent in the same button run.
    activate_at = body.index('data-profile-action="activate"')
    delete_at = body.index('data-profile-action="delete"')
    # Delete is nested inside the cluster span — the cluster is not a sibling
    # token that happens to appear somewhere after Activate.
    cluster_at = body.index('class="action-cluster"')
    cluster_close = body.index('</span>', cluster_at)
    assert cluster_at < delete_at < cluster_close


def test_recent_results_empty_state_guides_the_operator():
    """Regression pin — already green before Task 5; the behaviour must not regress."""
    js = _read(TUNING_JS)
    # Both empty-state branches must live in renderResults, not only one of them:
    # nothing-selected AND no-runs-yet. An OR over the two would stay green when
    # one branch is deleted.
    body = js.split("function renderResults", 1)[1].split("function loadResults", 1)[0]
    assert "Select a profile version" in body
    assert "No tests yet" in body
    html = _read(TUNING_HTML)
    assert 'id="profile-results"' in html


def _render_results_body() -> str:
    # NB: renderResults has nested `function ` tokens (runs.filter(function (r),
    # avgOf's pick callbacks), so split("function ", 1)[0] truncates. Bound by
    # the next top-level function — loadResults is defined immediately after.
    # Comment-stripped: product comments name the Number(x || 0) trap, and an
    # `assert "|| 0" not in body` must bite on code, not documentation.
    js = _read(TUNING_JS)
    return _js_code(js.split("function renderResults", 1)[1].split("function loadResults", 1)[0])


def _result_card_body() -> str:
    # resultCard is bounded by the next top-level function, renderResults.
    # It has no nested `function `, but the comment block between them would
    # otherwise be swallowed — bound explicitly, as with renderResults/loadResults.
    js = _read(TUNING_JS)
    return _js_code(js.split("function resultCard", 1)[1].split("function renderResults", 1)[0])


def _load_results_body() -> str:
    # loadResults' map callbacks contain nested `function `, so split on the
    # next top-level name (refresh) rather than the next `function `. Bound on
    # the signature: a preceding doc comment quotes "function loadResults" and
    # would otherwise capture from mid-comment.
    js = _read(TUNING_JS)
    return _js_code(js.split("async function loadResults(profile)", 1)[1].split("async function refresh", 1)[0])


def test_results_summary_fields_are_real_metrics():
    """The summary strip must show real metrics computed from the fetched runs.
    Averages must never default a missing field to zero — that would crown an
    incomplete run a perfect result. Regression pin for the labels live
    tuning.js already rendered; 'Test count' is the Task 5 addition."""
    body = _render_results_body()
    for label in ("Test count", "Average error", "Average overshoot",
                  "Average dispense time", "Success rate"):
        assert label in body, label
    # Averages must be computed over real fields, never defaulted to zero.
    assert "|| 0" not in body, "a missing metric must render —, not 0"


def test_avg_of_returns_null_when_no_finite_value():
    """avgOf must return null on an empty finite set, never 0. Returning 0
    would crown an all-missing batch a perfect average — the exact defect class
    this plan forbids."""
    body = _render_results_body()
    assert "function avgOf" in body
    avg_of = body.split("function avgOf", 1)[1].split("\n    }", 1)[0]
    assert "filter(Number.isFinite)" in avg_of
    assert "? vals.reduce" in avg_of and "/ vals.length : null" in avg_of
    assert ": null" in avg_of
    assert ": 0" not in avg_of, "avgOf must fall back to null, not 0"


def test_summary_metrics_render_em_dash_not_zero_when_average_absent():
    """Each averaged metric must fall back to '—' when its average is null.
    A null-to-'0.0 g' substitution reads as a perfect result."""
    body = _render_results_body()
    for expr in (
        "avgErr === null ? '—'",
        "avgOver === null ? '—'",
        "avgDur === null ? '—'",
        "rate === null ? '—'",
    ):
        assert expr in body, expr
    assert "avgErr === null ? '0" not in body
    assert "avgOver === null ? '0" not in body
    assert "avgDur === null ? '0" not in body


def test_result_card_coerces_missing_measurements_to_nan_not_zero():
    """resultCard must send null/undefined through NaN so the finite test can
    fall back to '—'. Number(null) === 0 would render a missing measurement as
    '0.0 g' and look like a perfect run."""
    body = _result_card_body()
    assert "(run.final_weight_g === null || run.final_weight_g === undefined) ? NaN : Number(run.final_weight_g)" in body
    assert "(run.final_error_g === null || run.final_error_g === undefined) ? NaN : Number(run.final_error_g)" in body
    assert "Number.isFinite(finalG) ? finalG.toFixed(1) + ' g' : '—'" in body
    assert "Number.isFinite(errG) ? errG.toFixed(1) + ' g' : '—'" in body
    assert "|| 0" not in _js_code(body)
    assert "Number(run.final_weight_g)" in body  # present, but only after the NaN guard


def test_load_results_averages_only_finite_values():
    """The Measured performance strip averages outside renderResults. Same
    rule: coerce missing to NaN, filter on Number.isFinite, average to null
    when nothing is left. Number(x || 0) here would silently fabricate."""
    body = _load_results_body()
    assert "(run.final_error_g === null || run.final_error_g === undefined) ? NaN : Math.abs(Number(run.final_error_g))" in body
    assert "(run.overshoot_g === null || run.overshoot_g === undefined) ? NaN : Number(run.overshoot_g)" in body
    assert body.count(".filter(Number.isFinite)") >= 2
    assert "errVals.length ? errVals.reduce" in body
    assert "overVals.length ? overVals.reduce" in body
    assert "avgError === null ? '—'" in body
    assert "avgOvershoot === null ? '—'" in body
    assert "|| 0" not in _js_code(body)


def test_versioning_semantics_unchanged():
    """Regression pin — already green before Task 5; the behaviour must not regress."""
    js = _read(TUNING_JS)
    # Capture updateEditBanner: a doc comment elsewhere in the file says
    # "Save as vN+1" and would satisfy a file-wide substring search.
    body = _js_code(js.split("function updateEditBanner", 1)[1].split("function startEdit", 1)[0])
    # Whole statements, not bare substrings: 'Save as version' contains
    # 'Save as v', so a prefix match stays green on a renamed button.
    assert "button.textContent = 'Save as v' + next;" in body
    assert "button.textContent = 'Save New Version';" in body


def test_kg_does_not_render_a_missing_target_as_zero():
    """kg('')/null/undefined/non-finite → 'No target'; a genuine 0 stays
    '0.0 kg'. Pinned as the two statements whole: token asserts are satisfied
    by comments (a revert to Number(grams) with the comments left in place
    stays green), and the zero path must not be gated on g !== 0."""
    js = _read(TUNING_JS)
    body = _js_code(js.split("function kg", 1)[1].split("function post", 1)[0])
    assert "var g = (grams === null || grams === undefined || grams === '') ? NaN : Number(grams);" in body
    assert "return Number.isFinite(g) ? (g / 1000).toFixed(1) + ' kg' : 'No target';" in body
    assert "g !== 0" not in body, "a genuine 0 must render 0.0 kg, not fall to 'No target'"
    assert "|| 0" not in body


def test_delete_is_offered_only_on_non_active_rows():
    """Delete is a dead affordance on an active version: the API returns 409
    (see test_delete_active_profile_version_is_rejected). The UI must not
    offer it there. Regression pin for the gate — test_delete_is_not_beside_apply
    captures the function body and cannot see which branch renders at runtime,
    so a future edit can re-surface Delete on active rows without failing it."""
    js = _read(TUNING_JS)
    body = js.split("function profileActions", 1)[1].split("function renderProfiles", 1)[0]
    assert "profile.active" in body
    cluster_at = body.index('class="action-cluster"')
    import re
    # The cluster string must be the FALSE-BRANCH VALUE of a `profile.active`
    # ternary, not a sibling concatenated after it and not the branch of some
    # other flag. `(profile.active ? '' : '') + cluster` keeps a profile.active
    # token near the cluster and keeps Delete nested inside it, so window
    # asserts and the nesting check both stay green; `otherFlag ? '' : cluster`
    # keeps the shape and drops the gate. Pin the whole contiguous gate.
    assert re.search(
        r"profile\.active\s*\?\s*''\s*:\s*' <span class=\"action-cluster\"",
        body,
    ), (
        "the action-cluster string must be the false-branch value of a "
        "profile.active ternary — (profile.active ? '' : '') + cluster is an "
        "unconditional cluster wearing a dummy ternary, and another flag's "
        "ternary is not this row's gate"
    )
    # And Delete must live inside that cluster, not merely after it.
    cluster_close = body.index("</span>", cluster_at)
    delete_at = body.index('data-profile-action="delete"')
    assert cluster_at < delete_at < cluster_close, (
        "the Delete button must be nested inside the action-cluster span"
    )
