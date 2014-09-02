-- Copyright (c) 2014 Microsoft Corporation. All rights reserved.
-- Released under Apache 2.0 license as described in the file LICENSE.
-- Author: Leonardo de Moura

-- logic.axioms.classical
-- ======================

import logic.core.quantifiers logic.core.cast struc.relation

using eq_ops

axiom prop_complete (a : Prop) : a = true ∨ a = false

theorem cases (P : Prop → Prop) (H1 : P true) (H2 : P false) (a : Prop) : P a :=
or_elim (prop_complete a)
  (assume Ht : a = true,  Ht⁻¹ ▸ H1)
  (assume Hf : a = false, Hf⁻¹ ▸ H2)

theorem cases_on (a : Prop) {P : Prop → Prop} (H1 : P true) (H2 : P false) : P a :=
cases P H1 H2 a

-- this supercedes the em in decidable
theorem em (a : Prop) : a ∨ ¬a :=
or_elim (prop_complete a)
  (assume Ht : a = true,  or_inl (eq_true_elim Ht))
  (assume Hf : a = false, or_inr (eq_false_elim Hf))

theorem prop_complete_swapped (a : Prop) : a = false ∨ a = true :=
cases (λ x, x = false ∨ x = true)
  (or_inr (refl true))
  (or_inl (refl false))
  a

theorem propext {a b : Prop} (Hab : a → b) (Hba : b → a) : a = b :=
or_elim (prop_complete a)
  (assume Hat,  or_elim (prop_complete b)
    (assume Hbt,  Hat ⬝ Hbt⁻¹)
    (assume Hbf, false_elim (a = b) (Hbf ▸ (Hab (eq_true_elim Hat)))))
  (assume Haf, or_elim (prop_complete b)
    (assume Hbt,  false_elim (a = b) (Haf ▸ (Hba (eq_true_elim Hbt))))
    (assume Hbf, Haf ⬝ Hbf⁻¹))

theorem iff_to_eq {a b : Prop} (H : a ↔ b) : a = b :=
iff_elim (assume H1 H2, propext H1 H2) H

theorem iff_eq_eq {a b : Prop} : (a ↔ b) = (a = b) :=
propext
  (assume H, iff_to_eq H)
  (assume H, eq_to_iff H)

using relation
theorem iff_congruence [instance] (P : Prop → Prop) : congruence iff iff P :=
congruence_mk
  (take (a b : Prop),
    assume H : a ↔ b,
    show P a ↔ P b, from eq_to_iff (subst (iff_to_eq H) (refl (P a))))