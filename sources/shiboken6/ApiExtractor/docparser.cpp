// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "docparser.h"
#include "abstractmetaargument.h"
#include "abstractmetaenum.h"
#include "abstractmetafunction.h"
#include "abstractmetalang.h"
#include "abstractmetatype.h"
#include "messages.h"
#include "modifications.h"
#include "reporthandler.h"
#include "enumtypeentry.h"
#include "complextypeentry.h"
#include "xmlutils.h"

#include <QtCore/QBuffer>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QTextStream>

#include "qtcompat.h"

#include <cstdlib>
#ifdef HAVE_LIBXSLT
#  include <libxslt/xsltutils.h>
#  include <libxslt/transform.h>
#endif

#include <algorithm>

using namespace Qt::StringLiterals;

static inline bool isXpathDocModification(const DocModification &mod)
{
    return mod.mode() == TypeSystem::DocModificationXPathReplace;
}

static inline bool isNotXpathDocModification(const DocModification &mod)
{
    return mod.mode() != TypeSystem::DocModificationXPathReplace;
}

static void removeXpathDocModifications(DocModificationList *l)
{
    l->erase(std::remove_if(l->begin(), l->end(), isXpathDocModification), l->end());
}

static void removeNonXpathDocModifications(DocModificationList *l)
{
    l->erase(std::remove_if(l->begin(), l->end(), isNotXpathDocModification), l->end());
}

DocParser::DocParser()
{
#ifdef HAVE_LIBXSLT
    xmlSubstituteEntitiesDefault(1);
#endif
}

DocParser::~DocParser() = default;

QString DocParser::getDocumentation(const XQueryPtr &xquery, const QString& query,
                                    const DocModificationList& mods)
{
    QString doc = execXQuery(xquery, query);
    return applyDocModifications(mods, doc.trimmed());
}

QString DocParser::execXQuery(const XQueryPtr &xquery, const QString& query)
{
    QString errorMessage;
    const QString result = xquery->evaluate(query, &errorMessage);
    if (!errorMessage.isEmpty())
        qCWarning(lcShibokenDoc, "%s", qPrintable(errorMessage));
    return result;
}

static bool usesRValueReference(const AbstractMetaArgument &a)
{
    return a.type().referenceType() == RValueReference;
}

bool DocParser::skipForQuery(const AbstractMetaFunctionCPtr &func)
{
    // Skip private functions and copies created by AbstractMetaClass::fixFunctions()
    // Note: Functions inherited from templates will cause warnings about missing
    // documentation, but they should at least be listed.
    if (!func || func->isPrivate()
        || func->attributes().testFlag(AbstractMetaFunction::AddedMethod)
        || func->isModifiedRemoved()
        || func->declaringClass() != func->ownerClass()
        || func->isConversionOperator()) {
        return true;
    }
    switch (func->functionType()) {
    case AbstractMetaFunction::MoveConstructorFunction:
    case AbstractMetaFunction::AssignmentOperatorFunction:
    case AbstractMetaFunction::MoveAssignmentOperatorFunction:
        return true;
    default:
        break;
    }

    return std::any_of(func->arguments().cbegin(), func->arguments().cend(),
                       usesRValueReference);
}

DocModificationList DocParser::getDocModifications(const AbstractMetaClassCPtr &cppClass)

{
    auto result = cppClass->typeEntry()->docModifications();
    removeXpathDocModifications(&result);
    return result;
}

static void filterBySignature(const AbstractMetaFunctionCPtr &func, DocModificationList *l)
{
    if (!l->isEmpty()) {
        const QString minimalSignature = func->minimalSignature();
        const auto filter = [&minimalSignature](const DocModification &mod) {
            return mod.signature() != minimalSignature;
        };
        l->erase(std::remove_if(l->begin(), l->end(), filter), l->end());
    }
}

DocModificationList DocParser::getDocModifications(const AbstractMetaFunctionCPtr &func,
                                                   const AbstractMetaClassCPtr &cppClass)
{
    DocModificationList result;
    if (func->isUserAdded()) {
        result = func->addedFunctionDocModifications();
        removeXpathDocModifications(&result);
    } else if (cppClass != nullptr) {
        result = cppClass->typeEntry()->functionDocModifications();
        removeXpathDocModifications(&result);
        filterBySignature(func, &result);
    }
    return result;
}

DocModificationList DocParser::getXpathDocModifications(const AbstractMetaClassCPtr &cppClass)
{
    auto result = cppClass->typeEntry()->docModifications();
    removeNonXpathDocModifications(&result);
    return result;
}

DocModificationList DocParser::getXpathDocModifications(const AbstractMetaFunctionCPtr &func,
                                                        const AbstractMetaClassCPtr &cppClass)
{
    DocModificationList result;
    if (func->isUserAdded()) {
        result = func->addedFunctionDocModifications();
        removeNonXpathDocModifications(&result);
    } else if (cppClass != nullptr) {
        result = cppClass->typeEntry()->functionDocModifications();
        removeNonXpathDocModifications(&result);
        filterBySignature(func, &result);
    }
    return result;
}

QString DocParser::enumBaseClass(const AbstractMetaEnum &e)
{
    switch (e.typeEntry()->pythonEnumType()) {
    case TypeSystem::PythonEnumType::IntEnum:
        return u"IntEnum"_s;
    case TypeSystem::PythonEnumType::Flag:
        return u"Flag"_s;
    case TypeSystem::PythonEnumType::IntFlag:
        return u"IntFlag"_s;
    default:
        break;
    }
    return e.typeEntry()->flags() != nullptr ? u"Flag"_s : u"Enum"_s;
}

AbstractMetaFunctionCList DocParser::documentableFunctions(const AbstractMetaClassCPtr &metaClass)
{
    auto result = metaClass->functionsInTargetLang();
    for (auto i = result.size() - 1; i >= 0; --i)  {
        if (DocParser::skipForQuery(result.at(i)) || result.at(i)->isUserAdded())
            result.removeAt(i);
    }
    result.append(metaClass->cppSignalFunctions());
    return result;
}

QString DocParser::applyDocModifications(const DocModificationList& xpathMods,
                                         const QString& xml)
{
    const char xslPrefix[] =
R"(<xsl:template match="/">
    <xsl:apply-templates />
</xsl:template>
<xsl:template match="*">
<xsl:copy>
    <xsl:copy-of select="@*"/>
    <xsl:apply-templates/>
</xsl:copy>
</xsl:template>
)";

    if (xpathMods.isEmpty() || xml.isEmpty())
        return xml;

    QString xsl = QLatin1StringView(xslPrefix);
    for (const DocModification &mod : xpathMods) {
        Q_ASSERT(isXpathDocModification(mod));
        QString xpath = mod.xpath();
        xpath.replace(u'"', u"&quot;"_s);
        xsl += "<xsl:template match=\""_L1 + xpath + "\">"_L1
               + mod.code() + "</xsl:template>\n"_L1;
    }

    QString errorMessage;
    const QString result = xsl_transform(xml, xsl, &errorMessage);
    if (!errorMessage.isEmpty())
        qCWarning(lcShibokenDoc, "%s",
                  qPrintable(msgXpathDocModificationError(xpathMods, errorMessage)));
    if (result == xml) {
        const QString message = u"Query did not result in any modifications to \""_s
            + xml + u'"';
        qCWarning(lcShibokenDoc, "%s",
                  qPrintable(msgXpathDocModificationError(xpathMods, message)));
    }
    return result;
}
