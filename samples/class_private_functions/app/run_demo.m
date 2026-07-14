vault = securepkg.Vault(5);
private_value = vault.reveal();
[vault, bumped_value] = vault.bump();
private_after_bump = vault.reveal();
function_choice = vault.functionChoice();
dot_choice = vault.dotChoice();
static_value = securepkg.Vault.staticValue();
